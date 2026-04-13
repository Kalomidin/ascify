/*
Copyright (c) 2015 - present Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include <algorithm>
#include <string>
#include "AscifyAction.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#if LLVM_VERSION_MAJOR < 17
#include "clang/Basic/TargetInfo.h"
#endif
#include "LLVMCompat.h"
#include "ArgParse.h"
#include "ImplicitCudaHeaders.h"
#include "CUDA2DPP.h"

using namespace ascify;

const std::string sDPP = "DPP";
const std::string s_string_literal = "[string literal]";
// Matchers' names
const StringRef sCudaLaunchKernel = "cudaLaunchKernel";

void AscifyAction::RewriteString(StringRef s, clang::SourceLocation start) {
  auto &SM = getCompilerInstance().getSourceManager();
  size_t begin = 0;
  while ((begin = s.find("cu", begin)) != StringRef::npos) {
    const size_t end = s.find_first_of(" ", begin + 4);
    StringRef name = s.slice(begin, end);
    const auto found = CUDA_RENAMES_MAP().find(name);
    if (found != CUDA_RENAMES_MAP().end()) {
      StringRef repName = found->second.dppName;
      dppCounter counter = {s_string_literal, ConvTypes::CONV_LITERAL, ApiTypes::API_RUNTIME, found->second.supportDegree};
      Statistics::current().incrementCounter(counter, name.str());
      if (!Statistics::isUnsupported(counter)) {
        clang::SourceLocation sl = start.getLocWithOffset(begin + 1);
        ct::Replacement Rep(SM, sl, name.size(), repName.str());
        clang::FullSourceLoc fullSL(sl, SM);
        insertReplacement(Rep, fullSL);
      }
    }
    if (end == StringRef::npos) break;
    begin = end + 1;
  }
}

// TODO
clang::SourceLocation AscifyAction::GetSubstrLocation(const std::string &str, const clang::SourceRange &sr) {
  clang::SourceLocation sl(sr.getBegin());
  return sl;
}

/**
  * Look at, and consider altering, a given token.
  *
  * If it's not a CUDA identifier, nothing happens.
  * If it's an unsupported CUDA identifier, a warning is emitted.
  * Otherwise, the source file is updated with the corresponding hipification.
  */
void AscifyAction::RewriteToken(const clang::Token &t) {
  if(!AscifyAMAP) {
    clang::SourceRange sr(t.getLocation());
    for(const auto &skipped: SkippedSourceRanges) {
      if(skipped.fullyContains(sr))
        return;
    }
  }

  // String literals containing CUDA references need fixing.
  if (t.is(clang::tok::string_literal)) {
    StringRef s(t.getLiteralData(), t.getLength());
    RewriteString(unquoteStr(s), t.getLocation());
    return;
  } else if (!t.isAnyIdentifier()) {
    // If it's neither a string nor an identifier, we don't care.
    return;
  }
  StringRef name = t.getRawIdentifier();
  clang::SourceLocation sl = t.getLocation();
  FindAndReplace(name, sl, CUDA_RENAMES_MAP());
}

void AscifyAction::FindAndReplace(StringRef name,
                                  clang::SourceLocation sl,
                                  const std::map<StringRef, dppCounter> &repMap,
                                  bool bReplace) {
  const auto found = repMap.find(name);
  if (found == repMap.end()) {
    // So it's an identifier, but not CUDA? Boring.
    return;
  }
  Statistics::current().incrementCounter(found->second, name.str());
  clang::DiagnosticsEngine &DE = getCompilerInstance().getDiagnostics();

  // Warn about the deprecated identifier in CUDA but hipify it.
  if (Statistics::isCudaDeprecated(found->second)) {
    const auto ID = DE.getCustomDiagID(clang::DiagnosticsEngine::Warning, "'%0' is deprecated in CUDA.");
    DE.Report(sl, ID) << found->first;
  }

  // TODO: Similar to hipify, add statistics analysis

    // Warn about the unsupported identifier.
  if (Statistics::isUnsupported(found->second)) {
    std::string sWarn = sDPP;
    const auto ID = DE.getCustomDiagID(clang::DiagnosticsEngine::Warning, "'%0' is unsupported in '%1'.");
    DE.Report(sl, ID) << found->first << sWarn;
    return;
  }
  if (!bReplace) {
    return;
  }
  StringRef repName = found->second.dppName;
  auto &SM = getCompilerInstance().getSourceManager();
  ct::Replacement Rep(SM, sl, name.size(), repName.str());
  clang::FullSourceLoc fullSL(sl, SM);
  insertReplacement(Rep, fullSL);
}

namespace {

clang::SourceRange getReadRange(clang::SourceManager &SM, const clang::SourceRange &exprRange) {
  clang::SourceLocation begin = exprRange.getBegin();
  clang::SourceLocation end = exprRange.getEnd();
  bool beginSafe = !SM.isMacroBodyExpansion(begin) || clang::Lexer::isAtStartOfMacroExpansion(begin, SM, clang::LangOptions{});
  bool endSafe = !SM.isMacroBodyExpansion(end) || clang::Lexer::isAtEndOfMacroExpansion(end, SM, clang::LangOptions{});
  if (beginSafe && endSafe) {
    return {SM.getFileLoc(begin), SM.getFileLoc(end)};
  } else {
    return {SM.getSpellingLoc(begin), SM.getSpellingLoc(end)};
  }
}

clang::SourceRange getWriteRange(clang::SourceManager &SM, const clang::SourceRange &exprRange) {
  clang::SourceLocation begin = exprRange.getBegin();
  clang::SourceLocation end = exprRange.getEnd();
  // If the range is contained within a macro, update the macro definition.
  // Otherwise, use the file location and hope for the best.
  if (!SM.isMacroBodyExpansion(begin) || !SM.isMacroBodyExpansion(end)) {
    return {SM.getFileLoc(begin), SM.getFileLoc(end)};
  }
  return {SM.getSpellingLoc(begin), SM.getSpellingLoc(end)};
}

StringRef readSourceText(clang::SourceManager &SM, const clang::SourceRange &exprRange) {
  return clang::Lexer::getSourceText(clang::CharSourceRange::getTokenRange(getReadRange(SM, exprRange)), SM, clang::LangOptions(), nullptr);
}

/**
  * Get a string representation of the expression `arg`, unless it's a defaulting function
  * call argument, in which case get a 0. Used for building argument lists to kernel calls.
  */
std::string stringifyZeroDefaultedArg(clang::SourceManager &SM, const clang::Expr *arg) {
  if (clang::isa<clang::CXXDefaultArgExpr>(arg)) return "0";
  else return std::string(readSourceText(SM, arg->getSourceRange()));
}

} // anonymous namespace

bool AscifyAction::Exclude(const dppCounter &hipToken) {
  return false;
}

void AscifyAction::InclusionDirective(clang::SourceLocation hash_loc,
                                      const clang::Token&,
                                      StringRef file_name,
                                      bool is_angled,
                                      clang::CharSourceRange filename_range,
                                      const clang::FileEntry*, StringRef,
                                      StringRef, const clang::Module*) {
                                        outs() << "File included: " << file_name << "\n";
  auto &SM = getCompilerInstance().getSourceManager();
  if (!SM.isWrittenInMainFile(hash_loc)) return;
  if (!firstHeader) {
    firstHeader = true;
    firstHeaderLoc = hash_loc;
  }


  const auto found = CUDA_INCLUDE_MAP.find(file_name);
  if (found == CUDA_INCLUDE_MAP.end()) return;
  bool exclude = Exclude(found->second);
  Statistics::current().incrementCounter(found->second, file_name.str());
  clang::SourceLocation sl = filename_range.getBegin();

  if (Statistics::isUnsupported(found->second)) {
    clang::DiagnosticsEngine &DE = getCompilerInstance().getDiagnostics();
    std::string sWarn = sDPP;
    const auto ID = DE.getCustomDiagID(clang::DiagnosticsEngine::Warning, "'%0' is unsupported header in '%1'.");
    DE.Report(sl, ID) << found->first << sWarn;
    return;
  }
  clang::StringRef newInclude;
  // Keep the same include type that the user gave.
  if (!exclude) {
    clang::SmallString<128> includeBuffer;
    llvm::StringRef name = found->second.dppName;
    if (is_angled) newInclude = llvm::Twine("<" + name+ ">").toStringRef(includeBuffer);
    else           newInclude = llvm::Twine("\"" + name + "\"").toStringRef(includeBuffer);
  } else {
    // hashLoc is location of the '#', thus replacing the whole include directive by empty newInclude starting with '#'.
    sl = hash_loc;
  }
  const char *B = SM.getCharacterData(sl);
  const char *E = SM.getCharacterData(filename_range.getEnd());
  ct::Replacement Rep(SM, sl, E - B, newInclude.str());
  insertReplacement(Rep, clang::FullSourceLoc{sl, SM});
}

void AscifyAction::PragmaDirective(clang::SourceLocation Loc, clang::PragmaIntroducerKind Introducer) {

}

bool AscifyAction::cudaLaunchKernel(const mat::MatchFinder::MatchResult &Result) {
  return true;
}

bool AscifyAction::cudaDeviceFuncCall(const mat::MatchFinder::MatchResult &Result) {
  return true;
}

bool AscifyAction::cubNamespacePrefix(const mat::MatchFinder::MatchResult &Result) {
  return true;
}

bool AscifyAction::cubUsingNamespaceDecl(const mat::MatchFinder::MatchResult &Result) {
  return false;
}

bool AscifyAction::cubFunctionTemplateDecl(const mat::MatchFinder::MatchResult &Result) {
  return false;
}

bool AscifyAction::cudaHostFuncCall(const mat::MatchFinder::MatchResult &Result) {
  return false;
}

bool AscifyAction::cudaOverloadedHostFuncCall(const mat::MatchFinder::MatchResult& Result) {
  return false;
}

bool AscifyAction::half2Member(const mat::MatchFinder::MatchResult &Result) {
  return false;
}

bool AscifyAction::dataTypeSelection(const mat::MatchFinder::MatchResult &Result) {
  return false;
}

void AscifyAction::insertReplacement(const ct::Replacement &rep, const clang::FullSourceLoc &fullSL) {
  llcompat::insertReplacement(*replacements, rep);
  if (PrintStats || PrintStatsCSV) {
    rep.getLength();
    Statistics::current().lineTouched(fullSL.getExpansionLineNumber());
    Statistics::current().bytesChanged(rep.getLength());
  }
}
std::unique_ptr<clang::ASTConsumer> AscifyAction::CreateASTConsumer(clang::CompilerInstance &CI, StringRef) {
  Finder.reset(new mat::MatchFinder);
  // Replace the <<<...>>> language extension with a hip kernel launch
  Finder->addMatcher(mat::cudaKernelCallExpr(mat::isExpansionInMainFile()).bind(sCudaLaunchKernel), this);
  return Finder->newASTConsumer();
}

void AscifyAction::Ifndef(clang::SourceLocation Loc, const clang::Token &MacroNameTok, const clang::MacroDefinition &MD) {
}

void AscifyAction::EndSourceFileAction() {
}

namespace {

/**
  * A silly little class to proxy PPCallbacks back to the AscifyAction class.
  */
class PPCallbackProxy : public clang::PPCallbacks {
  AscifyAction &ascifyAction;

public:
  explicit PPCallbackProxy(AscifyAction &action): ascifyAction(action) {}
  void InclusionDirective(clang::SourceLocation hash_loc, const clang::Token &include_token,
                          StringRef file_name, bool is_angled, clang::CharSourceRange filename_range,
#if LLVM_VERSION_MAJOR < 15
                          const clang::FileEntry *file,
#elif LLVM_VERSION_MAJOR == 15
                          Optional<clang::FileEntryRef> file,
#else
                          clang::OptionalFileEntryRef file,
#endif
                          StringRef search_path, StringRef relative_path,
#if LLVM_VERSION_MAJOR < 19
                          const clang::Module *SuggestedModule
#else
                          const clang::Module *SuggestedModule,
                          bool ModuleImported
#endif
#if LLVM_VERSION_MAJOR > 6
                        , clang::SrcMgr::CharacteristicKind FileType
#endif
                         ) override {
#if LLVM_VERSION_MAJOR < 15
    auto f = file;
#else
    auto f = &file->getFileEntry();
#endif
    ascifyAction.InclusionDirective(hash_loc, include_token, file_name, is_angled, filename_range, f, search_path, relative_path, SuggestedModule);
  }

  void PragmaDirective(clang::SourceLocation Loc, clang::PragmaIntroducerKind Introducer) override {
    ascifyAction.PragmaDirective(Loc, Introducer);
  }

  void Ifndef(clang::SourceLocation Loc, const clang::Token &MacroNameTok, const clang::MacroDefinition &MD) override {
    ascifyAction.Ifndef(Loc, MacroNameTok, MD);
  }

  void SourceRangeSkipped(clang::SourceRange Range, clang::SourceLocation EndifLoc) override {
    ascifyAction.AddSkippedSourceRange(Range);
  }
};
}

bool AscifyAction::BeginInvocation(clang::CompilerInstance &CI) {
  llcompat::RetainExcludedConditionalBlocks(CI);
  return true;
}

void AscifyAction::ExecuteAction() {
  clang::Preprocessor &PP = getCompilerInstance().getPreprocessor();
  // Register yourself as the preprocessor callback, by proxy.
  PP.addPPCallbacks(std::unique_ptr<PPCallbackProxy>(new PPCallbackProxy(*this)));
#if LLVM_VERSION_MAJOR > 3
  Statistics::cudaVersionUsedByClang = Statistics::convertCudaToolkitVersion(clang::ToCudaVersion(PP.getTargetInfo().getSDKVersion()));
  llvm::errs() << " !!!!!!! CUDA SDK version detected: " << int(clang::ToCudaVersion(PP.getTargetInfo().getSDKVersion())) << "\n";
#endif
  // Now we're done futzing with the lexer, have the subclass proceeed with Sema and AST matching.
  clang::ASTFrontendAction::ExecuteAction();
  auto &SM = getCompilerInstance().getSourceManager();
  // Start lexing the specified input file.
  llcompat::Memory_Buffer FromFile = llcompat::getMemoryBuffer(SM);
  clang::Lexer RawLex(SM.getMainFileID(), FromFile, SM, PP.getLangOpts());
  RawLex.SetKeepWhitespaceMode(true);
  // Perform a token-level rewrite of CUDA identifiers to hip ones. The raw-mode lexer gives us enough
  // information to tell the difference between identifiers, string literals, and "other stuff". It also
  // ignores preprocessor directives, so this transformation will operate inside preprocessor-deleted code.
  clang::Token RawTok;
  RawLex.LexFromRawLexer(RawTok);
  while (RawTok.isNot(clang::tok::eof)) {
    RewriteToken(RawTok);
    RawLex.LexFromRawLexer(RawTok);
  }
}

void AscifyAction::AddSkippedSourceRange(clang::SourceRange Range) {
  SkippedSourceRanges.push_back(Range);
}

void AscifyAction::run(const mat::MatchFinder::MatchResult &Result) {
  if (cudaLaunchKernel(Result)) return;
  if (cudaHostFuncCall(Result)) return;
  if (cudaOverloadedHostFuncCall(Result)) return;
  if (cudaDeviceFuncCall(Result)) return;
  if (cubNamespacePrefix(Result)) return;
  if (cubFunctionTemplateDecl(Result)) return;
  if (cubUsingNamespaceDecl(Result)) return;
  if (!NoUndocumented && half2Member(Result)) return;
}

