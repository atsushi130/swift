//===--- IRGenDebugInfo.h - Debug Info Support-------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file defines IR codegen support for debug information.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IRGEN_DEBUGINFO_H
#define SWIFT_IRGEN_DEBUGINFO_H

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/Support/ValueHandle.h"
#include "llvm/DebugInfo.h"
#include "llvm/DIBuilder.h"
#include "llvm/Support/Allocator.h"

#include "swift/SIL/SILLocation.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/AST/Stmt.h"

#include "DebugTypeInfo.h"
#include "IRBuilder.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"

#include <set>

namespace llvm {
  class DIBuilder;
}

namespace clang {
  class TargetInfo;
}

namespace swift {

class ASTContext;
class SILArgument;
class SILDebugScope;
class SILModule;
class AllocStackInst;

namespace irgen {

class Options;
class IRGenFunction;

typedef struct {
  unsigned Line, Col;
  const char* Filename;
} Location;

typedef struct {
  Location LocForLinetable, Loc;
} FullLocation;


enum IndirectionKind: bool { DirectValue = false, IndirectValue = true };
enum ArtificialKind : bool { RealValue = false, ArtificialValue = true };
enum IntrinsicKind  : bool { Declare = false, Value = true };

/// IRGenDebugInfo - Helper object that keeps track of the current
/// CompileUnit, File, LexicalScope, and translates SILLocations into
/// <llvm::DebugLoc>s.
class IRGenDebugInfo {
  const Options &Opts;
  const clang::TargetInfo &TargetInfo;
  SourceManager &SM;
  llvm::Module &M;
  llvm::DIBuilder DBuilder;
  IRGenModule &IGM;

  // Various caches.
  llvm::DenseMap<SILDebugScope *, llvm::WeakVH> ScopeCache;
  llvm::DenseMap<const char *, llvm::WeakVH> DIFileCache;
  llvm::DenseMap<TypeBase*, llvm::WeakVH> DITypeCache;
  std::map<std::string, llvm::WeakVH> DINameSpaceCache;
  llvm::DITypeIdentifierMap DIRefMap;

  // Subprograms need their scope to be RAUW'd when we work through
  // the list of imports.
  std::map<StringRef, llvm::WeakVH> Functions;

  // These are used by getArgNo.
  SILFunction *LastFn;
  SILBasicBlock::const_bbarg_iterator LastArg, LastEnd;
  unsigned LastArgNo;

  llvm::SmallString<256> MainFilename;
  StringRef CWDName; /// The current working directory.
  llvm::BumpPtrAllocator DebugInfoNames;
  llvm::DICompileUnit TheCU;
  llvm::DIFile MainFile;
  TypeAliasDecl *MetadataTypeDecl; /// The type decl for swift.type.

  FullLocation LastLoc; /// The last location that was emitted.
  SILDebugScope *LastScope; /// The scope of that last location.

  SmallVector<std::pair<FullLocation, SILDebugScope*>, 8>
  LocationStack; /// Used by pushLoc.

public:
  IRGenDebugInfo(const Options &Opts,
                 const clang::TargetInfo &TargetInfo,
                 IRGenModule &IGM,
                 llvm::Module &M);

  /// Finalize the llvm::DIBuilder owned by this object.
  void finalize();

  /// Update the IRBuilder's current debug location to the location
  /// Loc and the lexical scope DS.
  void setCurrentLoc(IRBuilder &Builder, SILDebugScope *DS,
                     Optional<SILLocation> Loc = Nothing);

  void clearLoc(IRBuilder &Builder) {
    LastLoc = {};
    LastScope = nullptr;
    Builder.SetCurrentDebugLocation(llvm::DebugLoc());
  }


  /// Push the current debug location onto a stack and initialize the
  /// IRBuilder to an empty location.
  void pushLoc() {
    LocationStack.push_back(std::make_pair(LastLoc, LastScope));
    LastLoc = {};
    LastScope = nullptr;
    //Builder.SetCurrentDebugLocation(DebugLoc());
  }

  /// Restore the current debug location from the stack.
  void popLoc() {
    std::tie(LastLoc, LastScope) = LocationStack.pop_back_val();
  }

  /// Emit debug info for an import declaration.
  void emitImport(ImportDecl *D);

  /// Emit debug info for the given function.
  /// \param DS The parent scope of the function.
  /// \param Fn The IR representation of the function.
  /// \param CC The calling convention of the function.
  /// \param Ty The signature of the function.
  void emitFunction(SILModule &SILMod, SILDebugScope *DS, llvm::Function *Fn,
                    AbstractCC CC, SILType Ty, DeclContext *DeclCtx = nullptr);

  /// Emit debug info for a given SIL function.
  void emitFunction(SILFunction *SILFn, llvm::Function *Fn);


  /// Convenience function useful for functions without any source
  /// location. Internally calls emitFunction, emits a debug
  /// scope, and finally sets it using setCurrentLoc.
  inline void emitArtificialFunction(IRGenFunction &IGF, llvm::Function *Fn,
                                     SILType SILTy = SILType()) {
    emitArtificialFunction(*IGF.IGM.SILMod, IGF.Builder, Fn, SILTy);
  }
  void emitArtificialFunction(SILModule &SILMod, IRBuilder &Builder,
                              llvm::Function *Fn, SILType SILTy = SILType());

  /// Emit a dbg.declare instrinsic at the current insertion point and
  /// the Builder's current debug location.
  /// \param Tag The DWARF tag that should be used.
  void emitVariableDeclaration(IRBuilder& Builder,
                               llvm::Value *Storage,
                               DebugTypeInfo Ty,
                               StringRef Name,
                               unsigned Tag,
                               unsigned ArgNo = 0,
                               IndirectionKind = DirectValue,
                               ArtificialKind = RealValue,
                               IntrinsicKind = Declare);

  /// Convenience function for stack-allocated variables. Calls
  /// emitVariableDeclaration internally.
  void emitStackVariableDeclaration(IRBuilder& Builder,
                                    llvm::Value *Storage,
                                    DebugTypeInfo Ty,
                                    StringRef Name,
                                    swift::SILInstruction *I,
                                    IndirectionKind Indirection = DirectValue);

  /// Convenience function for variables that are function arguments.
  void emitArgVariableDeclaration(IRBuilder& Builder,
                                  llvm::Value *Storage,
                                  DebugTypeInfo Ty,
                                  StringRef Name,
                                  unsigned ArgNo,
                                  IndirectionKind Indirection = DirectValue,
                                  IntrinsicKind = Declare);

  /// Emit debug metadata for a global variable.
  void emitGlobalVariableDeclaration(llvm::GlobalValue *Storage,
                                     StringRef Name,
                                     StringRef LinkageName,
                                     DebugTypeInfo DebugType,
                                     Optional<SILLocation> Loc);

  /// Emit debug metadata for type metadata (for generic types). So meta.
  void emitTypeMetadata(IRGenFunction &IGF, llvm::Value *Metadata,
                        StringRef Name);

  /// Return the native, absolute path to the main file.
  StringRef getMainFilename() const { return MainFilename; }

  /// Return the DIBuilder.
  llvm::DIBuilder &getBuilder() { return DBuilder; }

private:
  StringRef BumpAllocatedString(const char* Data, size_t Length);
  StringRef BumpAllocatedString(std::string S);
  StringRef BumpAllocatedString(StringRef S);

  llvm::DIType createType(DebugTypeInfo DbgTy, llvm::DIDescriptor Scope,
                          llvm::DIFile File);
  llvm::DIType getOrCreateType(DebugTypeInfo DbgTy, llvm::DIDescriptor Scope);
  llvm::DIDescriptor getOrCreateScope(SILDebugScope *DS);
  StringRef getCurrentDirname();
  llvm::DIFile getOrCreateFile(const char *Filename);
  llvm::DIType getOrCreateDesugaredType(Type Ty, DebugTypeInfo DTI,
                                        llvm::DIDescriptor Scope);
  StringRef getName(const FuncDecl& FD);
  StringRef getName(SILLocation L);
  StringRef getMangledName(DebugTypeInfo DTI);
  llvm::DIArray createParameterTypes(CanSILFunctionType FnTy,
                                     llvm::DIDescriptor Scope,
                                     DeclContext *DeclCtx);
  llvm::DIArray createParameterTypes(SILType SILTy,
                                     llvm::DIDescriptor Scope,
                                     DeclContext *DeclCtx);
  void createParameterType(llvm::SmallVectorImpl<llvm::Value*>& Parameters,
                           SILType CanTy, llvm::DIDescriptor Scope,
                           DeclContext* DeclCtx);
  llvm::DIArray getTupleElements(TupleType *TupleTy, llvm::DIDescriptor Scope,
                                 llvm::DIFile File, unsigned Flags,
                                 DeclContext* DeclContext);
  unsigned getArgNo(SILFunction *Fn, SILArgument *Arg);
  llvm::DIFile getFile(llvm::DIDescriptor Scope);
  llvm::DIScope getOrCreateNamespace(llvm::DIScope Namespace,
                                     std::string MangledName,
                                     llvm::DIFile File, unsigned Line = 0);
  llvm::DIScope getNamespace(StringRef MangledName);
  llvm::DIArray getStructMembers(NominalTypeDecl *D, llvm::DIDescriptor Scope,
                                 llvm::DIFile File, unsigned Flags);
  llvm::DICompositeType createStructType(DebugTypeInfo DbgTy,
                                         NominalTypeDecl *Decl,
                                         StringRef Name,
                                         llvm::DIDescriptor Scope,
                                         llvm::DIFile File, unsigned Line,
                                         unsigned SizeInBits,
                                         unsigned AlignInBits,
                                         unsigned Flags,
                                         llvm::DIType DerivedFrom,
                                         unsigned RuntimeLang);
  llvm::DIDerivedType createMemberType(DebugTypeInfo DTI,
                                       unsigned &OffsetInBits,
                                       llvm::DIDescriptor Scope,
                                       llvm::DIFile File,
                                       unsigned Flags);
  llvm::DIArray getEnumElements(DebugTypeInfo DbgTy,
                                 EnumDecl *D,
                                 llvm::DIDescriptor Scope,
                                 llvm::DIFile File,
                                 unsigned Flags);
  llvm::DICompositeType
  createEnumType(DebugTypeInfo DbgTy, EnumDecl *Decl, StringRef Name,
                  llvm::DIDescriptor Scope, llvm::DIFile File, unsigned Line,
                  unsigned Flags);
  bool emitVarDeclForSILArgOrNull(IRBuilder& Builder,
                                  llvm::Value *Storage,
                                  DebugTypeInfo Ty,
                                  StringRef Name,
                                  SILFunction *Fn,
                                  SILValue Value,
                                  IndirectionKind Indirection);
  TypeAliasDecl *getMetadataType();
};

/// ArtificialLocation - An RAII object that temporarily switches to
/// an artificial debug location that has a valid scope, but no line
/// information. This is useful when emitting compiler-generated
/// instructions (e.g., ARC-inserted calls to release()) that have no
/// source location associated with them. The DWARF specification
/// allows the compiler to use the special line number 0 to indicate
/// code that can not be attributed to any source location.
class ArtificialLocation {
  IRGenDebugInfo *DI;
public:
  /// Set the current location to line 0, but within the current scope
  /// (= the top of the LexicalBlockStack).
  ArtificialLocation(IRGenDebugInfo *DI, IRBuilder &Builder) : DI(DI) {
    if (DI) {
      DI->pushLoc();
      llvm::DIDescriptor Scope(Builder.getCurrentDebugLocation().
                               getScope(Builder.getContext()));
      auto DL = llvm::DebugLoc::get(0, 0, Scope);
      Builder.SetCurrentDebugLocation(DL);
    }
  }

  /// ~ArtificialLocation - Autorestore everything back to normal.
  ~ArtificialLocation() {
    if (DI) DI->popLoc();
  }
};


} // irgen
} // swift

#endif
