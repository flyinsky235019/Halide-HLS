#include "CodeGen_Internal.h"
#include "Log.h"
#include "LLVM_Headers.h"

using std::string;
using std::map;
using std::vector;
using namespace llvm;

namespace Halide {
namespace Internal {

void CodeGen::Closure::visit(const Let *op) {
    op->value.accept(this);
    ignore.push(op->name, 0);
    op->body.accept(this);
    ignore.pop(op->name);
}

void CodeGen::Closure::visit(const LetStmt *op) {
    op->value.accept(this);
    ignore.push(op->name, 0);
    op->body.accept(this);
    ignore.pop(op->name);
}

void CodeGen::Closure::visit(const For *op) {
    ignore.push(op->name, 0);
    op->min.accept(this);
    op->extent.accept(this);
    op->body.accept(this);
    ignore.pop(op->name);
}

void CodeGen::Closure::visit(const Load *op) {
    op->index.accept(this);
    if (!ignore.contains(op->name)) {
        log(3) << "Adding " << op->name << " to closure\n";
        // result[op->name + ".host"] = gen->llvm_type_of(op->type)->getPointerTo();
        reads[op->name] = op->type;
    } else {
        log(3) << "Not adding " << op->name << " to closure\n";
    }
}

void CodeGen::Closure::visit(const Store *op) {
    op->index.accept(this);
    op->value.accept(this);
    if (!ignore.contains(op->name)) {
        log(3) << "Adding " << op->name << " to closure\n";
        // result[op->name + ".host"] = gen->llvm_type_of(op->value.type())->getPointerTo();
        writes[op->name] = op->value.type();
    } else {
        log(3) << "Not adding " << op->name << " to closure\n";
    }
}

void CodeGen::Closure::visit(const Allocate *op) {
    ignore.push(op->name, 0);
    op->size.accept(this);
    op->body.accept(this);
    ignore.pop(op->name);
}

void CodeGen::Closure::visit(const Variable *op) {            
    if (ignore.contains(op->name)) {
        log(3) << "Not adding " << op->name << " to closure\n";
    } else {
        log(3) << "Adding " << op->name << " to closure\n";
        vars[op->name] = op->type;
    }
}

CodeGen::Closure::Closure(Stmt s, const string &loop_variable) {
    ignore.push(loop_variable, 0);
    s.accept(this);
}

vector<llvm::Type*> CodeGen::Closure::llvm_types(CodeGen *gen) {
    vector<llvm::Type *> res;
    map<string, Type>::const_iterator iter;
    for (iter = vars.begin(); iter != vars.end(); ++iter) {
        res.push_back(gen->llvm_type_of(iter->second));
    }
    for (iter = reads.begin(); iter != reads.end(); ++iter) {
        res.push_back(gen->llvm_type_of(iter->second)->getPointerTo());
    }
    for (iter = writes.begin(); iter != writes.end(); ++iter) {
        res.push_back(gen->llvm_type_of(iter->second)->getPointerTo());
    }
    return res;
}

vector<string> CodeGen::Closure::names() {
    vector<string> res;
    map<string, Type>::const_iterator iter;
    for (iter = vars.begin(); iter != vars.end(); ++iter) {
        log(2) << "vars:  " << iter->first << "\n";
        res.push_back(iter->first);
    }
    for (iter = reads.begin(); iter != reads.end(); ++iter) {
        log(2) << "reads: " << iter->first << "\n";
        res.push_back(iter->first + ".host");
    }
    for (iter = writes.begin(); iter != writes.end(); ++iter) {
        log(2) << "writes: " << iter->first << "\n";
        res.push_back(iter->first + ".host");
    }
    return res;
}

StructType *CodeGen::Closure::build_type(CodeGen *gen) {
    StructType *struct_t = StructType::create(*gen->context, "closure_t");
    struct_t->setBody(llvm_types(gen), false);
    return struct_t;
}

void CodeGen::Closure::pack_struct(CodeGen *gen, Value *dst, const Scope<Value *> &src, IRBuilder<> *builder) {
    // dst should be a pointer to a struct of the type returned by build_type
    int idx = 0;
    vector<string> nm = names();
    vector<llvm::Type*> ty = llvm_types(gen);
    for (size_t i = 0; i < nm.size(); i++) {
        Value *val = src.get(nm[i]);
        Value *ptr = builder->CreateConstInBoundsGEP2_32(dst, 0, idx++);
        if (val->getType() != ty[i]) {
            val = builder->CreateBitCast(val, ty[i]);
        }            
        builder->CreateStore(val, ptr);
    }
}

void CodeGen::Closure::unpack_struct(CodeGen *gen, Scope<Value *> &dst, Value *src, IRBuilder<> *builder, Module *module, LLVMContext &context) {
    // src should be a pointer to a struct of the type returned by build_type
    int idx = 0;
    vector<string> nm = names();
    for (size_t i = 0; i < nm.size(); i++) {
        Value *ptr = builder->CreateConstInBoundsGEP2_32(src, 0, idx++);
        LoadInst *load = builder->CreateLoad(ptr);
        Value *val = load;
        if (load->getType()->isPointerTy()) {
            // Give it a unique type so that tbaa tells llvm that this can't alias anything
            load->setMetadata("tbaa", MDNode::get(context, vec<Value *>(MDString::get(context, nm[i]))));
            
            llvm::Function *fn = module->getFunction("force_no_alias");
            assert(fn && "Did not find force_no_alias in initial module");
            Value *arg = builder->CreatePointerCast(load, llvm::Type::getInt8Ty(context)->getPointerTo());
            CallInst *call = builder->CreateCall(fn, vec(arg));
            mark_call_return_no_alias(call, context);
            val = builder->CreatePointerCast(call, val->getType());
            
        }
        dst.push(nm[i], val);
        val->setName(nm[i]);
    }
}

}
}