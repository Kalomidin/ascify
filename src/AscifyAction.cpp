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
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/Lexer.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#if LLVM_VERSION_MAJOR < 17
#include "clang/Basic/TargetInfo.h"
#endif
#include "LLVMCompat.h"
#include "ArgParse.h"
#include "ImplicitCudaHeaders.h"
#include "CUDA2DPP.h"
#include "StringUtils.h"

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

static bool isDeclSpecifierLike(const clang::Token &t) {
  if (t.is(clang::tok::star))
    return true;
  if (t.isAnyIdentifier())
    return true;
  switch (t.getKind()) {
  case clang::tok::kw_struct:
  case clang::tok::kw_class:
  case clang::tok::kw_union:
  case clang::tok::kw_enum:
  case clang::tok::kw_const:
  case clang::tok::kw_volatile:
  case clang::tok::kw_restrict:
  case clang::tok::kw_unsigned:
  case clang::tok::kw_signed:
  case clang::tok::kw_void:
  case clang::tok::kw_bool:
  case clang::tok::kw_char:
  case clang::tok::kw_short:
  case clang::tok::kw_int:
  case clang::tok::kw_long:
  case clang::tok::kw_float:
  case clang::tok::kw_double:
  case clang::tok::kw_wchar_t:
  case clang::tok::kw_char16_t:
  case clang::tok::kw_char32_t:
  case clang::tok::kw_auto:
    return true;
  default:
    return false;
  }
}

int getNextNonZeroIdx(const std::deque<clang::Token> &rawTokenWindow, int &idx) {
  if (idx == 0) return 0;
  idx--;
  while(idx > 0 && rawTokenWindow[idx].getKind() == 0) {
    idx--;
  }
  return idx;
}


/// We should support two cases:
/// 1. ( ... ) malloc
/// -> currentIdx = getIdx("(")
/// 2. malloc
/// -> currentIdx = getIdx("malloc")
static bool matchParenCastBeforeMalloc(const std::deque<clang::Token> &win, int mallocIdx, int &currentIdx) {
  if (mallocIdx <= 0)
    return false;
  int castCloseIdx = getNextNonZeroIdx(win, mallocIdx);
  // if it is case 2, return true
  if (!win[castCloseIdx].is(clang::tok::r_paren)) {
    // check if the token is equal sign
    if (!win[castCloseIdx].is(clang::tok::equal)) {
      return false;
    }
    currentIdx = mallocIdx;
    return true;
  }
  int depth = 1;
  std::size_t j = castCloseIdx;
  while (j > 0) {
    --j;
    if (win[j].is(clang::tok::r_paren))
      depth++;
    else if (win[j].is(clang::tok::l_paren)) {
      if(j == 0) {
        return false;
      }
      depth--;
      if (depth == 0) {
        currentIdx = j;
        return true;
      }
    }
  }
  return false;
}

static unsigned ascifySourceOffset(const clang::SourceManager &SM, clang::SourceLocation loc) {
  return SM.getDecomposedLoc(SM.getFileLoc(loc)).second;
}

static bool scanMallocCallAndSemi(const char *p, llvm::StringRef &argOut, const char *&afterSemiOut) {
  while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
    ++p;
  if (*p != '(')
    return false;
  ++p;
  const char *argBegin = p;
  int depth = 1;
  while (*p && depth > 0) {
    if (*p == '"') {
      ++p;
      while (*p && *p != '"') {
        if (*p == '\\' && p[1])
          ++p;
        ++p;
      }
      if (*p == '"')
        ++p;
      continue;
    }
    if (*p == '\'') {
      ++p;
      while (*p && *p != '\'') {
        if (*p == '\\' && p[1])
          ++p;
        ++p;
      }
      if (*p == '\'')
        ++p;
      continue;
    }
    if (*p == '/' && p[1] == '/') {
      while (*p && *p != '\n')
        ++p;
      continue;
    }
    if (*p == '/' && p[1] == '*') {
      p += 2;
      while (*p && !(*p == '*' && p[1] == '/'))
        ++p;
      if (*p == '*' && p[1] == '/')
        p += 2;
      continue;
    }
    if (*p == '(')
      ++depth;
    else if (*p == ')')
      --depth;
    ++p;
  }
  if (depth != 0)
    return false;
  const char *argEnd = p - 1;
  argOut = llvm::StringRef(argBegin, argEnd - argBegin);
  while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
    ++p;
  if (*p != ';')
    return false;
  ++p;
  afterSemiOut = p;
  return true;
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

bool AscifyAction::tryRewriteMallocDeviceAllocDecl(clang::Lexer &lex, clang::Token &tok) {
  if (!tok.isAnyIdentifier() || tok.getRawIdentifier() != "malloc")
    return false;
  if (rawTokenWindow.empty()) {
    llvm::errs() << "error: rawTokenWindow is empty\n";
    return false;
  }
  const std::size_t mi = rawTokenWindow.size() - 1;
  if (!(rawTokenWindow[mi].getLocation() == tok.getLocation())) {
    llvm::errs() << "error: mi location mismatch\n";
    return false;
  }

  int currentIdx = mi;
  if (!matchParenCastBeforeMalloc(rawTokenWindow, mi, currentIdx)) {
    llvm::errs() << "error: matchParenCastBeforeMalloc failed\n";
    return false;
  }
  if (currentIdx < 2) {
    llvm::errs() << "error: currentIdx < 2\n";
    return false;
  }
  int equalIdx = getNextNonZeroIdx(rawTokenWindow, currentIdx);
  if (!rawTokenWindow[equalIdx].is(clang::tok::equal)) {
    llvm::errs() << "error: equal sign not found\n";
    return false;
  }
  const std::size_t varIdx = getNextNonZeroIdx(rawTokenWindow, currentIdx);
  if (!rawTokenWindow[varIdx].isAnyIdentifier()) {
    llvm::errs() << "error: variable not found\n";
    return false;
  }

  std::size_t declStartIdx = varIdx;
  for (int t = varIdx; t > 0;) {
    --t;
    if (!isDeclSpecifierLike(rawTokenWindow[t]))
      break;
    declStartIdx = t;
  }

  clang::CompilerInstance &CI = getCompilerInstance();
  clang::SourceManager &SM = CI.getSourceManager();
  const clang::LangOptions &LO = CI.getLangOpts();
  clang::SourceLocation declStartLoc = rawTokenWindow[declStartIdx].getLocation();
  if (!SM.isWrittenInMainFile(declStartLoc) || !SM.isWrittenInMainFile(tok.getLocation()))
    return false;

  if (!AscifyAMAP) {
    clang::SourceRange sr(declStartLoc);
    for (const auto &skipped : SkippedSourceRanges) {
      if (skipped.fullyContains(sr))
        return false;
    }
  }

  const clang::SourceLocation mallocEnd =
      clang::Lexer::getLocForEndOfToken(tok.getLocation(), 0, SM, LO);
  const char *scan = SM.getCharacterData(mallocEnd);
  llvm::StringRef mallocArg;
  const char *afterSemi = nullptr;
  if (!scanMallocCallAndSemi(scan, mallocArg, afterSemi))
    return false;

  const clang::FileID mainFID = SM.getMainFileID();
  const char *mainBuf = SM.getCharacterData(SM.getLocForStartOfFile(mainFID));
  const char *declCh = SM.getCharacterData(declStartLoc);
  if (declCh < mainBuf || afterSemi < declCh)
    return false;

  const unsigned endOff = static_cast<unsigned>(afterSemi - mainBuf);
  clang::SourceLocation varLoc = rawTokenWindow[varIdx].getLocation();
  const clang::SourceLocation varEnd =
      clang::Lexer::getLocForEndOfToken(varLoc, 0, SM, LO);
  llvm::StringRef declPart =
      clang::Lexer::getSourceText(clang::CharSourceRange::getCharRange(declStartLoc, varEnd), SM, LO);

  llvm::SmallString<512> newText;
  newText += declPart;
  newText += ";\naclrtMallocHost(";
  newText += rawTokenWindow[varIdx].getRawIdentifier();
  newText += ", ";
  newText += mallocArg.trim();
  newText += ");";

  // Only replace when the span [decl, ';') fully covers this `malloc` spelling so the
  // initializer (including malloc) is removed and not left behind beside aclrtMalloc.
  const clang::SourceLocation mallocPast =
      clang::Lexer::getLocForEndOfToken(tok.getLocation(), 0, SM, LO);
  const char *mallocFirst = SM.getCharacterData(tok.getLocation());
  const char *mallocAfter = SM.getCharacterData(mallocPast);
  if (mallocFirst < declCh || mallocAfter > afterSemi)
    return false;
  if (llvm::StringRef(mallocFirst, static_cast<std::size_t>(mallocAfter - mallocFirst)) != "malloc")
    return false;

  const unsigned repLen = static_cast<unsigned>(afterSemi - declCh);
  ct::Replacement Rep(SM, declStartLoc, repLen, newText.str());
  insertReplacement(Rep, clang::FullSourceLoc(declStartLoc, SM));

  while (!tok.is(clang::tok::eof)) {
    if (ascifySourceOffset(SM, tok.getLocation()) >= endOff)
      break;
    lex.LexFromRawLexer(tok);
  }
  return true;
}

/**
  * Look at, and consider altering, a given token.
  *
  * If it's not a CUDA identifier, nothing happens.
  * If it's an unsupported CUDA identifier, a warning is emitted.
  * Otherwise, the source file is updated with the corresponding ascification.
  *
  * Returns true when the raw lexer was advanced past rewritten text (see malloc rewrite).
  */
bool AscifyAction::RewriteToken(clang::Lexer &lex, clang::Token &tok) {
  if (!AscifyAMAP) {
    clang::SourceRange sr(tok.getLocation());
    for (const auto &skipped : SkippedSourceRanges) {
      if (skipped.fullyContains(sr))
        return false;
    }
  }

  if (tok.is(clang::tok::string_literal)) {
    llvm::StringRef s(tok.getLiteralData(), tok.getLength());
    RewriteString(unquoteStr(s), tok.getLocation());
    return false;
  }
  if (!tok.isAnyIdentifier())
    return false;

  llvm::StringRef name = tok.getRawIdentifier();
  if (name == "malloc" && tryRewriteMallocDeviceAllocDecl(lex, tok))
    return true;

  FindAndReplace(name, tok.getLocation(), CUDA_RENAMES_MAP());
  return false;
}

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
  rawTokenWindow.clear();
  clang::Token RawTok;
  RawLex.LexFromRawLexer(RawTok);
  while (RawTok.isNot(clang::tok::eof)) {
    while (rawTokenWindow.size() >= kRawTokenWindowCap)
      rawTokenWindow.pop_front();
    rawTokenWindow.push_back(RawTok);
    if (RewriteToken(RawLex, RawTok))
      continue;
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

