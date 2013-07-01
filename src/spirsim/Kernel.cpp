#include "common.h"

#define __STDC_LIMIT_MACROS
#define __STDC_CONSTANT_MACROS
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Function.h"
#include "llvm/Metadata.h"
#include "llvm/Module.h"
#include "llvm/Type.h"

#include "CL/cl.h"
#include "Kernel.h"
#include "Memory.h"

using namespace spirsim;
using namespace std;

Kernel::Kernel(const llvm::Function *function, const llvm::Module *module)
{
  m_function = function;
  m_localMemory = 0;

  // Get name
  m_name = function->getName().str();

  // Get required work-group size from metadata
  memset(m_requiredWorkGroupSize, 0, sizeof(size_t[3]));
  llvm::NamedMDNode *mdKernels = module->getNamedMetadata("opencl.kernels");
  if (mdKernels)
  {
    llvm::MDNode *md = mdKernels->getOperand(0);
    for (int i = 0; i < md->getNumOperands(); i++)
    {
      llvm::Value *op = md->getOperand(i);
      if (op->getValueID() == llvm::Value::MDNodeVal)
      {
        llvm::MDNode *val = ((llvm::MDNode*)op);
        string name = val->getOperand(0)->getName().str();
        if (name == "reqd_work_group_size")
        {
          for (int j = 0; j < 3; j++)
          {
            m_requiredWorkGroupSize[j] =
              ((const llvm::ConstantInt*)val->getOperand(j+1))->getZExtValue();
          }
        }
      }
    }
  }

  // Set-up global variables
  llvm::Module::const_global_iterator itr;
  for (itr = module->global_begin(); itr != module->global_end(); itr++)
  {
    llvm::PointerType *type = itr->getType();
    if (type->getPointerAddressSpace() == AddrSpaceLocal)
    {
      size_t size = getTypeSize(itr->getInitializer()->getType());
      TypedValue v = {
        sizeof(size_t),
        1,
        new unsigned char[sizeof(size_t)]
      };
      *((size_t*)v.data) = m_localMemory;
      m_localMemory += size;
      m_arguments[itr] = v;
    }
    else if (itr->isConstant())
    {
      m_constants.push_back(itr);
    }
  }
}

Kernel::~Kernel()
{
}

void Kernel::allocateConstants(Memory *memory)
{
  list<const llvm::GlobalVariable*>::const_iterator itr;
  for (itr = m_constants.begin(); itr != m_constants.end(); itr++)
  {
    const llvm::Constant *initializer = (*itr)->getInitializer();
    const llvm::Type *type = initializer->getType();

    // Allocate buffer
    size_t size = getTypeSize(type);
    TypedValue v = {
      sizeof(size_t),
      1,
      new unsigned char[sizeof(size_t)]
    };
    size_t address = memory->allocateBuffer(size);
    *((size_t*)v.data) = address;
    m_constantBuffers.push_back(address);
    m_arguments[*itr] = v;

    // Initialise buffer contents
    unsigned char *data = new unsigned char[getTypeSize(type)];
    getConstantData(data, (const llvm::Constant*)initializer);
    memory->store(data, address, size);
    delete[] data;
  }
}

void Kernel::deallocateConstants(Memory *memory)
{
  list<size_t>::const_iterator itr;
  for (itr = m_constantBuffers.begin(); itr != m_constantBuffers.end(); itr++)
  {
    memory->deallocateBuffer(*itr);
  }
  m_constantBuffers.clear();
}

const llvm::Argument* Kernel::getArgument(unsigned int index) const
{
  assert(index < getNumArguments());

  llvm::Function::const_arg_iterator argItr = m_function->arg_begin();
  for (int i = 0; i < index; i++)
  {
    argItr++;
  }
  return argItr;
}

size_t Kernel::getArgumentSize(unsigned int index) const
{
  const llvm::Type *type = getArgument(index)->getType();

  // Check if pointer argument
  if (type->isPointerTy())
  {
    return sizeof(size_t);
  }

  return getTypeSize(type);
}

unsigned int Kernel::getArgumentType(unsigned int index) const
{
  const llvm::Type *type = getArgument(index)->getType();

  // Check if scalar argument
  if (!type->isPointerTy())
  {
    return CL_KERNEL_ARG_ADDRESS_PRIVATE;
  }

  // Check address space
  unsigned addressSpace = type->getPointerAddressSpace();
  switch (addressSpace)
  {
  case AddrSpaceGlobal:
    return CL_KERNEL_ARG_ADDRESS_GLOBAL;
  case AddrSpaceConstant:
    return CL_KERNEL_ARG_ADDRESS_CONSTANT;
  case AddrSpaceLocal:
    return CL_KERNEL_ARG_ADDRESS_LOCAL;
  default:
    cerr << "Unrecognized address space " << addressSpace << endl;
    return 0;
  }
}

const llvm::Function* Kernel::getFunction() const
{
  return m_function;
}

size_t Kernel::getLocalMemorySize() const
{
  return m_localMemory;
}


const std::string& Kernel::getName() const
{
  return m_name;
}

unsigned int Kernel::getNumArguments() const
{
  return m_function->arg_size();
}

const size_t* Kernel::getRequiredWorkGroupSize() const
{
  return m_requiredWorkGroupSize;
}

void Kernel::setArgument(unsigned int index, TypedValue value)
{
  if (index >= m_function->arg_size())
  {
    cerr << "Argument index out of range." << endl;
    return;
  }

  unsigned int type = getArgumentType(index);
  if (type == CL_KERNEL_ARG_ADDRESS_LOCAL)
  {
    TypedValue v = {
      sizeof(size_t),
      1,
      new unsigned char[sizeof(size_t)]
    };
    *((size_t*)v.data) = m_localMemory;
    m_localMemory += value.size;

    m_arguments[getArgument(index)] = v;
  }
  else
  {
    const llvm::Type *type = getArgument(index)->getType();
    if (type->isVectorTy())
    {
      value.num = type->getVectorNumElements();
      value.size /= value.num;
    }
    m_arguments[getArgument(index)] = clone(value);
  }
}

TypedValueMap::const_iterator Kernel::args_begin() const
{
  return m_arguments.begin();
}

TypedValueMap::const_iterator Kernel::args_end() const
{
  return m_arguments.end();
}
