// LLVM module pass to get information from kernel functions.
// 
// Copyright (c) 2011 Universidad Rey Juan Carlos
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.


#include "pocl.h"
#include "Workgroup.h"
#include "llvm/Argument.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Function.h"
#include "llvm/GlobalVariable.h"
#include "llvm/Instructions.h"
#include "llvm/Module.h"
#include "llvm/Pass.h"
#include "llvm/PassSupport.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Transforms/Utils/Cloning.h"

using namespace std;
using namespace llvm;
using namespace pocl;

typedef SmallVector<std::pair<Function *, Function *>, 8> KernelPairVector;
static void regenerate_kernel_metadata(Module &M, KernelPairVector &kernels);

cl::opt<string>
Header("header",
       cl::desc("Output header file with kernel description macros"),
       cl::value_desc("header"));

namespace {
  class GenerateHeader : public ModulePass {
  
  public:
    static char ID;
    GenerateHeader() : ModulePass(ID) {}
    
    virtual void getAnalysisUsage(AnalysisUsage &AU) const;
    virtual bool runOnModule(Module &M);

  private:
    void ProcessPointers(Function *F,
                         raw_fd_ostream &out);
    Function *ProcessAutomaticLocals(Function *F,
                                     raw_fd_ostream &out);
  };
}

char GenerateHeader::ID = 0;
static RegisterPass<GenerateHeader> X("generate-header",
				      "Kernel information header creation pass");

void
GenerateHeader::getAnalysisUsage(AnalysisUsage &AU) const
{
  AU.addRequired<TargetData>();
}

bool
GenerateHeader::runOnModule(Module &M)
{
  bool changed = false;

  // store the new and old kernel pairs in order to regenerate
  // all the metadata that used to point to the unmodified
  // kernels
  KernelPairVector kernels;

  string ErrorInfo;
  raw_fd_ostream out(Header.c_str(), ErrorInfo, raw_fd_ostream::F_Append);

  for (Module::iterator mi = M.begin(), me = M.end(); mi != me; ++mi) {
    if (!Workgroup::isKernelToProcess(*mi))
      continue;
  
    Function *F = mi;

    ProcessPointers(F, out);
    
    Function *new_kernel = ProcessAutomaticLocals(F, out);
    if (new_kernel != F)
      changed = true;

    kernels.push_back(std::make_pair(F, new_kernel));
  }

  if (changed)
    {
      regenerate_kernel_metadata(M, kernels);

      /* Delete the old kernels. */
      for (KernelPairVector::const_iterator i = kernels.begin(),
             e = kernels.end(); i != e; ++i) 
        {
          Function *old_kernel = (*i).first;
          Function *new_kernel = (*i).second;
          if (old_kernel == new_kernel) continue;
          old_kernel->eraseFromParent();
        }
    }
  return changed;
}


void
GenerateHeader::ProcessPointers(Function *F,
                                raw_fd_ostream &out)
{
  int num_args = F->getFunctionType()->getNumParams();
    
  out << "#define _" << F->getName() << "_NUM_ARGS " << num_args << '\n';
      
  bool is_pointer[num_args];
  bool is_local[num_args];
    
  int i = 0;
  for (Function::const_arg_iterator ii = F->arg_begin(),
         ee = F->arg_end();
       ii != ee; ++ii) {
    Type *t = ii->getType();
    
    if (const PointerType *p = dyn_cast<PointerType> (t)) {
      is_pointer[i] = true;
      // index 0 is for function attributes, parameters start at 1.
      if (p->getAddressSpace() == POCL_ADDRESS_SPACE_GLOBAL ||
          p->getAddressSpace() == POCL_ADDRESS_SPACE_CONSTANT)
        is_local[i] = false;
      else
        is_local[i] = true;
    } else {
      is_pointer[i] = false;
      is_local[i] = false;
    }
    
    ++i;
  }
    
  out << "#define _" << F->getName() << "_ARG_IS_POINTER {";
  if (num_args != 0) {
    out << is_pointer[0];
    for (i = 1; i < num_args; ++i)
      out << ", " << is_pointer[i];
  }
  out << "}\n";
  
  out << "#define _" << F->getName() << "_ARG_IS_LOCAL {";
  if (num_args != 0) {
    out << is_local[0];
    for (i = 1; i < num_args; ++i)
      out << ", " << is_local[i];
  }
  out << "}\n";
}


Function *
GenerateHeader::ProcessAutomaticLocals(Function *F,
                                       raw_fd_ostream &out)
{
  Module *M = F->getParent();
  TargetData &TD = getAnalysis<TargetData>();
  
  
  SmallVector<GlobalVariable *, 8> locals;

  SmallVector<Type *, 8> parameters;
  for (Function::const_arg_iterator i = F->arg_begin(),
         e = F->arg_end();
       i != e; ++i)
    parameters.push_back(i->getType());
    
  for (Module::global_iterator i = M->global_begin(),
         e = M->global_end();
       i != e; ++i) {
    std::string funcName = "";
#ifdef LLVM_3_0
    funcName = F->getNameStr();
#else
    funcName = F->getName().str();
#endif
    if (i->getName().startswith(funcName + ".")) {
      // Additional checks might be needed here. For now
      // we assume any global starting with kernel name
      // is declaring a local variable.
      locals.push_back(i);
      // Add the parameters to the end of the function parameter list.
      parameters.push_back(i->getType());
    }
  }
    
  out << "#define _" << F->getName() << "_NUM_LOCALS "<< locals.size() << "\n";
  out << "#define _" << F->getName() << "_LOCAL_SIZE {";
  if (!locals.empty()) {
    out << TD.getTypeAllocSize(locals[0]->getInitializer()->getType());
    for (unsigned i = 1; i < locals.size(); ++i)
      out << ", " << TD.getTypeAllocSize(locals[i]->getInitializer()->getType());
  }
  out << "}\n";    

  if (locals.empty()) {
    // This kernel fingerprint has not changed.
    return F;
  }
  
  // Create the new function.
  FunctionType *ft = FunctionType::get(F->getReturnType(),
                                       parameters,
                                       F->isVarArg());
  Function *new_kernel = Function::Create(ft,
                                          F->getLinkage(),
                                          "",
                                          M);
  new_kernel->takeName(F);
  
  ValueToValueMapTy vv;
  Function::arg_iterator j = new_kernel->arg_begin();
  for (Function::const_arg_iterator i = F->arg_begin(),
         e = F->arg_end();
       i != e; ++i) {
    j->setName(i->getName());
    vv[i] = j;
    ++j;
  }
  
  for (int i = 0; j != new_kernel->arg_end(); ++i, ++j) {
    j->setName("_local" + Twine(i));
    vv[locals[i]] = j;
  }
                                 
  SmallVector<ReturnInst *, 1> ri;
  CloneFunctionInto(new_kernel, F, vv, false, ri);

  return new_kernel;
}


/**
 * Regenerates the metadata that points to the original kernel
 * (of which finger print was modified) to point to the new
 * kernel.
 *
 * Only checks if the first operand of the metadata is the kernel
 * function.
 */
void
regenerate_kernel_metadata(Module &M, KernelPairVector &kernels)
{
  // reproduce the opencl.kernel_wg_size_info metadata
  NamedMDNode *wg_sizes = M.getNamedMetadata("opencl.kernel_wg_size_info");
  if (wg_sizes != NULL && wg_sizes->getNumOperands() > 0) 
    {
      for (std::size_t mni = 0; mni < wg_sizes->getNumOperands(); ++mni)
        {
          MDNode *wgsizeMD = dyn_cast<MDNode>(wg_sizes->getOperand(mni));
          for (KernelPairVector::const_iterator i = kernels.begin(),
                 e = kernels.end(); i != e; ++i) 
            {
              Function *old_kernel = (*i).first;
              Function *new_kernel = (*i).second;
              if (old_kernel == new_kernel || wgsizeMD->getNumOperands() == 0 ||
                  dyn_cast<Function>(wgsizeMD->getOperand(0)) != old_kernel) 
                continue;
              // found a wg size metadata that points to the old kernel, copy its
              // operands except the first one to a new MDNode
              SmallVector<Value*, 8> operands;
              operands.push_back(new_kernel);
              for (int opr = 1; opr < wgsizeMD->getNumOperands(); ++opr)
                {
                  operands.push_back(wgsizeMD->getOperand(opr));
                }
              MDNode *new_wg_md = MDNode::get(M.getContext(), operands);
              wg_sizes->addOperand(new_wg_md);
            } 
        }
    }

  // reproduce the opencl.kernels metadata
  NamedMDNode *nmd = M.getNamedMetadata("opencl.kernels");
  if (nmd)
    M.eraseNamedMetadata(nmd);

  nmd = M.getOrInsertNamedMetadata("opencl.kernels");
  for (KernelPairVector::const_iterator i = kernels.begin(),
         e = kernels.end();
       i != e; ++i) 
    {
      MDNode *md = MDNode::get(M.getContext(), ArrayRef<Value *>((*i).second));
      nmd->addOperand(md);
    }
}
