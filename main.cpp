#include <llvm/ProfileData/Coverage/CoverageMapping.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <string>
#include <vector>
#include <sys/stat.h>
//---------------------------------------------------------------------------
// llvm-coverage-to-html converter
// (c) 2017 Thomas Neumann
// SPDX-License-Identifier: GPL-2.0-or-later
//---------------------------------------------------------------------------
using namespace std;
//---------------------------------------------------------------------------
static unique_ptr<llvm::coverage::CoverageMapping> loadCoverage(const string& objectFile, const string& profileFile) {
   auto fs = llvm::vfs::getRealFileSystem();;
   llvm::StringRef objectFiles[1] = {objectFile};
   auto res = llvm::coverage::CoverageMapping::load(objectFiles, profileFile, *fs);
   if (!res) {
      cerr << "unable to load profile" << endl;
      exit(1);
   }
   return move(*res);
}
//---------------------------------------------------------------------------
static void escapeHtml(ostream& out, string_view s)
// Write a string, escaping HTML as needed
{
   const char *current = s.data(), *end = s.data();
   for (char c : s) {
      string_view escaped;
      switch (c) {
         case '<': escaped = "&lt;"; break;
         case '>': escaped = "&gt;"; break;
         case '&': escaped = "&amp;"; break;
         case '\"': escaped = "&quot;"; break;
         case '\n':
         case '\r': escaped = " "; break;
         default: ++end; continue;
      }
      auto sv = string_view(current, end);
      out << sv << escaped;
      current = end = end + 1;
   }
   auto sv = string_view(current, end);
   out << sv;
}
//---------------------------------------------------------------------------
struct HitList {
   vector<unsigned> hits, misses;
};
using CoverageList = map<string, HitList>;
//---------------------------------------------------------------------------
class SourceWriter {
   private:
   ostream& out;
   CoverageList& coverageList;
   string fileName;
   struct Part {
      string str;
      unsigned count;
      bool hasCode;
      bool regionEntry;
   };
   vector<Part> parts;

   public:
   /// Statistics
   unsigned executableLines = 0, hitLines = 0;

   /// Constructor
   SourceWriter(ostream& out, CoverageList& coverageList, string fileName) : out(out), coverageList(coverageList), fileName(fileName) {}

   /// Add a fragment
   void addData(string str, unsigned count, bool hasData, bool regionEntry);

   /// Write the current line
   void finishLine(unsigned lineNo);
};
//---------------------------------------------------------------------------
void SourceWriter::addData(string str, unsigned count, bool hasData, bool regionEntry) {
   parts.push_back(Part{move(str), count, hasData, regionEntry});
}
//---------------------------------------------------------------------------
static bool isTrivialCode(const string& code)
// Check for trivial code
{
   static const auto trivialRegex = [] {
      return regex(
         // Completely empty
         R"(^\s*$)"
         "|"
         // Noop ;
         R"(^;$)"
         "|"
         // Brackets
         R"(^\s*[{}]*\s*$)"
         "|"
         // LLVM attributes C++11's "= default;" to the last t
         R"(^t$)");
   }();
   return regex_match(code, trivialRegex);
}
//---------------------------------------------------------------------------
void SourceWriter::finishLine(unsigned lineNo)
// Write the current line
{
   unsigned maxCount = 0, candidates = 0, hitCandidates = 0, regionEntry = 0;
   for (auto& p : parts) {
      bool code = p.hasCode, hit = p.count;
      if (isTrivialCode(p.str)) code = hit = false;
      if (code) candidates++;
      if (hit) hitCandidates++;
      if (p.regionEntry && p.count) regionEntry++;
      if (p.count > maxCount) maxCount = p.count;
   }
   if (candidates) {
      executableLines++;
      if (hitCandidates)
         hitLines++;
      auto& c = coverageList[fileName];
      (hitCandidates ? c.hits : c.misses).push_back(lineNo);
   }

   // Write the line intro
   if (!candidates) {
      out << R"(            )";
   } else if (candidates > hitCandidates) {
      if (!regionEntry) {
         out << R"(<span class="lineNoCov">)";
      } else {
         out << R"(<span class="linePartCov">)";
      }
      stringstream ss;
      ss << hitCandidates << " / " << candidates << " ";
      string s = ss.str();
      for (unsigned index = s.length(); index < 12; ++index)
         out << " ";
      out << s << "</span>";
   } else {
      stringstream ss;
      if (maxCount < 1000) {
         ss << maxCount;
      } else if (maxCount < 1000000) {
         ss << (maxCount / 1000) << "K";
      } else if (maxCount < 1000000000) {
         ss << (maxCount / 1000000) << "M";
      } else {
         ss << (maxCount / 1000000000) << "G";
      }
      string s = ss.str();
      for (unsigned index = s.length(); index < 12; ++index)
         out << " ";
      out << s << "</span>";
   }

   // Write the fragments
   out << " : ";
   unsigned mode = 0;
   for (auto& p : parts) {
      unsigned newMode;
      if (p.hasCode && !isTrivialCode(p.str)) {
         if (p.count) {
            newMode = (candidates > hitCandidates) ? 2 : 3;
         } else
            newMode = 1;
      } else {
         newMode = 0;
      }
      if (mode != newMode) {
         if (mode)
            out << "</span>";
         switch (newMode) {
            case 0: break;
            case 1: out << R"(<span class="lineNoCov">)"; break;
            case 2: out << R"(<span class="linePartCov">)"; break;
            case 3: out << R"(<span class="lineCov">)"; break;
         }
         mode = newMode;
      }
      escapeHtml(out, p.str);
   }
   parts.clear();
   if (mode)
      out << "</span>";
   out << endl;
}
//---------------------------------------------------------------------------
class SourceReader {
   private:
   struct LineInfo {
      string line;
      unsigned ignoreFrom, ignoreTo;
   };

   SourceWriter& out;
   vector<LineInfo> lines;
   unsigned lineNo, colPos;

   public:
   SourceReader(istream& in, SourceWriter& out, const vector<string>& extraIgnore);

   //// Skip to a position
   void skipTo(unsigned line, unsigned col, unsigned count, bool hasCode, unsigned regionEntry);
   /// Flush the rest
   void flush();
};
//---------------------------------------------------------------------------
SourceReader::SourceReader(istream& in, SourceWriter& out, const vector<string>& extraIgnore)
   : out(out), lineNo(0), colPos(0) {
   // Collect all lines
   string s;
   bool ignoreBlock = false;
   vector<unsigned> ignoreLines;
   while (getline(in, s)) {
      bool ignoreLine = false;
      if (ignoreBlock) {
         ignoreLine = true;
         if (s.find("LCOV_EXCL_STOP") != string::npos)
            ignoreBlock = false;
      } else {
         ignoreLine = false;
         if (s.find("LCOV_EXCL_START") != string::npos) {
            ignoreLine = true;
            ignoreBlock = true;
         } else if (s.find("LCOV_EXCL_LINE") != string::npos) {
            ignoreLines.push_back(lines.size());
            ignoreLine = true;
         } else if (any_of(extraIgnore.begin(), extraIgnore.end(), [&](const string& extraIgnore) { return (!extraIgnore.empty()) && (s.find(extraIgnore) != string::npos); })) {
            ignoreLines.push_back(lines.size());
            ignoreLine = true;
         }
      }

      unsigned ignoreFrom = 0, ignoreTo = 0;
      if (ignoreLine)
         ignoreTo = s.length() + 1;
      lines.push_back(LineInfo{move(s), ignoreFrom, ignoreTo});
   }

   // Single line comments cover the whole statement
   for (auto line : ignoreLines) {
      for (unsigned prev = line; prev > 0;) {
         auto& i = lines[--prev];
         auto& l = i.line;
         unsigned stop;
         for (stop = l.size(); stop; --stop) {
            char c = l[stop - 1];
            if ((c != ' ') && (c != '\t') && (c != '\n') && (c != '\r') && (c != '{') && (c != '}')) break;
         }
         bool willBreak = (stop > 0);
         // We also ignore the last semicolon due to clang strangeness
         if (stop && (l[stop - 1] == ';')) --stop;
         if (stop < l.size()) {
            if (i.ignoreFrom != i.ignoreTo) {
               i.ignoreFrom = 0; // We cannot handle more fine-grained ignores for now
               i.ignoreTo = l.size();
            } else {
               i.ignoreFrom = stop;
               i.ignoreTo = l.size();
            }
         }
         if (willBreak) break;
      }
      for (unsigned next = line + 1, limit = lines.size(); next < limit; ++next) {
         auto& i = lines[next];
         auto& l = i.line;
         unsigned stop = 0;
         for (unsigned limit = l.size(); stop < limit; stop++) {
            char c = l[stop];
            if ((c != ' ') && (c != '\t') && (c != '\n') && (c != '\r') && (c != '}')) break;
         }
         if (stop > 0) {
            if (i.ignoreFrom != i.ignoreTo) {
               i.ignoreFrom = 0; // We cannot handle more fine-grained ignores for now
               i.ignoreTo = l.size();
            } else {
               i.ignoreFrom = 0;
               i.ignoreTo = stop;
            }
         }
         if (stop < l.size()) break;
      }
   }
}
//---------------------------------------------------------------------------
static string getSubstr(const string& s, unsigned from, unsigned len)
// A substring that handles out-of-bounds more gracefully. Needed if the source code gets out of sync
{
   if (from >= s.length()) return {};
   if (s.length() - from <= len) return s.substr(from);
   return s.substr(from, len);
}
//---------------------------------------------------------------------------
void SourceReader::skipTo(unsigned targetLine, unsigned col, unsigned count, bool hasCode, unsigned regionEntry) {
   if (targetLine > lineNo) {
      if (lineNo) {
         if (colPos < lines[lineNo - 1].line.length()) {
            bool ignore = (lines[lineNo - 1].ignoreFrom <= colPos) && (colPos < lines[lineNo - 1].ignoreTo);
            out.addData(getSubstr(lines[lineNo - 1].line, colPos, ~0u), count, hasCode && (count || !ignore), lineNo == regionEntry);
         }
         out.finishLine(lineNo);
      }
      while (lineNo < targetLine) {
         if (lineNo >= lines.size())
            break;

         // Handle ignore settings
         auto& i = lines[lineNo];
         ++lineNo;
         colPos = 0;
         if (lineNo < targetLine) {
            if ((i.ignoreFrom != i.ignoreTo) && ((i.ignoreFrom != 0) || (i.ignoreTo != i.line.size()))) {
               if (i.ignoreFrom)
                  out.addData(getSubstr(i.line, 0, i.ignoreFrom), count, hasCode, lineNo == regionEntry);
               out.addData(getSubstr(i.line, i.ignoreFrom, i.ignoreTo - i.ignoreFrom), count, hasCode && count, lineNo == regionEntry);
               if (i.ignoreTo < i.line.length())
                  out.addData(getSubstr(i.line, i.ignoreTo, i.line.length()), count, hasCode, lineNo == regionEntry);
            } else {
               bool ignore = (i.ignoreFrom != i.ignoreTo);
               out.addData(i.line, count, hasCode && (count || !ignore), lineNo == regionEntry);
            }
            out.finishLine(lineNo);
         }
      }
   }
   if (col > colPos + 1) {
      bool ignore = (lines[lineNo - 1].ignoreFrom <= colPos) && (colPos < lines[lineNo - 1].ignoreTo);
      out.addData(getSubstr(lines[lineNo - 1].line, colPos, (col - 1) - colPos), count, hasCode && (count || !ignore), lineNo == regionEntry);
      colPos = col - 1;
   }
}
//---------------------------------------------------------------------------
void SourceReader::flush()
// Flush the rest
{
   if (lineNo) {
      out.addData(lines[lineNo - 1].line.substr(colPos), 0, false, 0);
      out.finishLine(lineNo);
      ++lineNo;
   }
   for (unsigned limit = lines.size(); lineNo <= limit; ++lineNo) {
      out.addData(lines[lineNo - 1].line, 0, false, 0);
      out.finishLine(lineNo);
   }
}
//---------------------------------------------------------------------------
static void processCode(ostream& out, CoverageList& coverageList, llvm::coverage::CoverageMapping& coverage, llvm::StringRef file, const vector<string>& extraIgnore, unsigned& hitLines, unsigned& executableLines)
// Process a file
{
   hitLines = executableLines = 0;
   ifstream in(file.str());
   if (!in.is_open()) {
      out << "<br/><h4>No source code found!</h4><br/>" << endl;
      return;
   }

   SourceWriter writer(out, coverageList, file.str());
   SourceReader reader(in, writer, extraIgnore);
   auto data = coverage.getCoverageForFile(file);
   unsigned currentCount = 0, regionEntry = 0;
   bool hasCode = false;
   for (auto& i : data) {
      reader.skipTo(i.Line, i.Col, currentCount, hasCode, regionEntry);
      currentCount = i.Count;
      hasCode = i.HasCount && (!i.IsGapRegion);
      regionEntry = i.IsRegionEntry ? i.Line : 0;
   }
   reader.flush();

   hitLines = writer.hitLines;
   executableLines = writer.executableLines;
}
//---------------------------------------------------------------------------
static inline unsigned computePerc(unsigned hitLines, unsigned executableLines)
// Compute percentage (x10)
{
   unsigned perc;
   if ((!hitLines) || (!executableLines)) {
      perc = 0;
   } else {
      perc = (hitLines * 1000 / executableLines);
      if (!perc) perc = 1;
   }
   return perc;
}
//---------------------------------------------------------------------------
static void writeHeader(ostream& out, const string& binaryName, const string& timestamp, const string& prettyFile, unsigned hitLines, unsigned executableLines)
// Write the HTML header
{
   out << R"(<!DOCTYPE HTML PUBLIC "-//W3C//DTD HTML 4.01 Transitional//EN>
             <html>
             <head>
                <title>Coverage - )";
   escapeHtml(out, binaryName);
   if (!prettyFile.empty()) {
      out << " - ";
      escapeHtml(out, prettyFile);
   }
   out << R"(</title>
                <link rel="stylesheet" type="text/css" href="llvmcov2html.css"/>
             </head>
             <body>
             <table width="100%" border="0" cellspacing="0" cellpadding="0">
             <tr><td class="title">Coverage Report</td></tr>
             <tr><td class="ruler"><img src="glass.png" width="3" height="3" alt=""/></td></tr>
             <tr>
             <td width="100%">
                <table cellpadding="1" border="0" width="100%">
                   <tr>
                      <td class="headerItem" width="20%">Command:</td>
                      <td class="headerValue" width="80%" colspan=6>)";
   escapeHtml(out, binaryName);
   out << "</td>" << endl;
   unsigned perc = computePerc(hitLines, executableLines);
   out << R"(        </tr>
                     <tr>
                     <td class="headerItem" width="20%">Date:</td>
                     <td class="headerValue" width="15%">)";
   escapeHtml(out, timestamp);
   out << R"(</td>
                     <td width="5%"></td>
                     <td class="headerItem" width="20%">Instrumented&nbsp;lines:</td>
                     <td class="headerValue" width="10%">)"
       << executableLines << R"(</td>
                   </tr>
                   <tr>
                   <td class="headerItem" width="20%">Code&nbsp;covered:
                   <td class="headerValue" width="15%">)"
       << (perc / 10) << "." << (perc % 10) << R"( %</td>
                   <td width="5%"></td>
                     <td class="headerItem" width="20%">Executed&nbsp;lines:</td>
                     <td class="headerValue" width="10%">)"
       << hitLines << R"(</td>
                   </tr>
                 </table>
               </td>
             </tr>
             <tr><td class="ruler"><img src="glass.png" width="3" height="3" alt=""/></td></tr>
           </table>)"
       << endl;
}
//---------------------------------------------------------------------------
static void writeFooter(ostream& out)
// Write the HTML footer
{
   out << R"(<table width="100%" border="0" cellspacing="0" cellpadding="0">
             <tr><td class="ruler"><img src="glass.png" width="3" height="3" alt=""/></td></tr>
             <tr><td class="versionInfo">Generated by: llvmcov2html</td></tr>
           </table>
           <br/>
           </body>
           </html>)"
       << endl;
}
//---------------------------------------------------------------------------
static bool processFile(CoverageList& coverageList, const string& outFile, llvm::coverage::CoverageMapping& coverage, llvm::StringRef file, const vector<string>& extraIgnore, unsigned& hitLines, unsigned& executableLines, const string& binaryName, const string& timestamp, const string& prettyFile)
// Process a file
{
   // Check the source code
   stringstream code;
   processCode(code, coverageList, coverage, file, extraIgnore, hitLines, executableLines);
   if (!executableLines)
      return false;

   // Write the header
   ofstream out(outFile);
   writeHeader(out, binaryName, timestamp, prettyFile, hitLines, executableLines);

   // Write the code
   out << R"(<pre class="source">)" << endl;
   out << code.str();
   out << "</pre>" << endl;

   // Write the footer
   writeFooter(out);
   return true;
}
//---------------------------------------------------------------------------
static void constructBar(ostream& out, unsigned perc)
// Construct a percentage bar
{
   double percent = perc / 10.0;
   const char* color;
   if (percent >= 75)
      color = "emerald.png";
   else if (percent >= 35)
      color = "amber.png";
   else
      color = "ruby.png";
   int width = static_cast<int>(percent + 0.5);

   char buffer[200];
   if (width < 1) {
      snprintf(buffer, sizeof(buffer), "<img src=\"snow.png\" width=\"%d\" height=\"10\" alt=\"%.1f%%\"/>", 100, 0.0);
   } else if (width >= 100) {
      snprintf(buffer, sizeof(buffer), "<img src=\"%s\" width=\"%d\" height=\"10\" alt=\"%.1f%%\"/>", color, 100, 100.0);
   } else {
      snprintf(buffer, sizeof(buffer), "<img src=\"%s\" width=\"%d\" height=\"10\" alt=\"%.1f%%\"/><img src=\"snow.png\" width=\"%d\" height=\"10\" alt=\"%.1f%%\"/>", color, width, percent, 100 - width, percent);
   }
   out << buffer;
}
//---------------------------------------------------------------------------
static void writeBLOB(const string& fileName, const unsigned char* data, unsigned len)
// Write binary data to a file
{
   ofstream out(fileName, ios::out | ios::binary);
   if (!out.is_open()) {
      cerr << "unable to write " << fileName << endl;
      exit(1);
   }
   out.write(reinterpret_cast<const char*>(data), len);
}
//---------------------------------------------------------------------------
static void writeExtras(string targetDir)
// Write extra files
{
   static const unsigned char ruby[] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x01, 0x03, 0x00, 0x00, 0x00, 0x25, 0xdb, 0x56, 0xca, 0x00, 0x00, 0x00, 0x07, 0x74, 0x49, 0x4d, 0x45, 0x07, 0xd2, 0x07, 0x11, 0x0f, 0x18, 0x10, 0x5d, 0x57, 0x34, 0x6e, 0x00, 0x00, 0x00, 0x09, 0x70, 0x48, 0x59, 0x73, 0x00, 0x00, 0x0b, 0x12, 0x00, 0x00, 0x0b, 0x12, 0x01, 0xd2, 0xdd, 0x7e, 0xfc, 0x00, 0x00, 0x00, 0x04, 0x67, 0x41, 0x4d, 0x41, 0x00, 0x00, 0xb1, 0x8f, 0x0b, 0xfc, 0x61, 0x05, 0x00, 0x00, 0x00, 0x06, 0x50, 0x4c, 0x54, 0x45, 0xff, 0x35, 0x2f, 0x00, 0x00, 0x00, 0xd0, 0x33, 0x9a, 0x9d, 0x00, 0x00, 0x00, 0x0a, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x60, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0xe5, 0x27, 0xde, 0xfc, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
   static const unsigned char amber[] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x01, 0x03, 0x00, 0x00, 0x00, 0x25, 0xdb, 0x56, 0xca, 0x00, 0x00, 0x00, 0x07, 0x74, 0x49, 0x4d, 0x45, 0x07, 0xd2, 0x07, 0x11, 0x0f, 0x28, 0x04, 0x98, 0xcb, 0xd6, 0xe0, 0x00, 0x00, 0x00, 0x09, 0x70, 0x48, 0x59, 0x73, 0x00, 0x00, 0x0b, 0x12, 0x00, 0x00, 0x0b, 0x12, 0x01, 0xd2, 0xdd, 0x7e, 0xfc, 0x00, 0x00, 0x00, 0x04, 0x67, 0x41, 0x4d, 0x41, 0x00, 0x00, 0xb1, 0x8f, 0x0b, 0xfc, 0x61, 0x05, 0x00, 0x00, 0x00, 0x06, 0x50, 0x4c, 0x54, 0x45, 0xff, 0xe0, 0x50, 0x00, 0x00, 0x00, 0xa2, 0x7a, 0xda, 0x7e, 0x00, 0x00, 0x00, 0x0a, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x60, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0xe5, 0x27, 0xde, 0xfc, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
   static const unsigned char emerald[] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x01, 0x03, 0x00, 0x00, 0x00, 0x25, 0xdb, 0x56, 0xca, 0x00, 0x00, 0x00, 0x07, 0x74, 0x49, 0x4d, 0x45, 0x07, 0xd2, 0x07, 0x11, 0x0f, 0x22, 0x2b, 0xc9, 0xf5, 0x03, 0x33, 0x00, 0x00, 0x00, 0x09, 0x70, 0x48, 0x59, 0x73, 0x00, 0x00, 0x0b, 0x12, 0x00, 0x00, 0x0b, 0x12, 0x01, 0xd2, 0xdd, 0x7e, 0xfc, 0x00, 0x00, 0x00, 0x04, 0x67, 0x41, 0x4d, 0x41, 0x00, 0x00, 0xb1, 0x8f, 0x0b, 0xfc, 0x61, 0x05, 0x00, 0x00, 0x00, 0x06, 0x50, 0x4c, 0x54, 0x45, 0x1b, 0xea, 0x59, 0x0a, 0x0a, 0x0a, 0x0f, 0xba, 0x50, 0x83, 0x00, 0x00, 0x00, 0x0a, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x60, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0xe5, 0x27, 0xde, 0xfc, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
   static const unsigned char snow[] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x01, 0x03, 0x00, 0x00, 0x00, 0x25, 0xdb, 0x56, 0xca, 0x00, 0x00, 0x00, 0x07, 0x74, 0x49, 0x4d, 0x45, 0x07, 0xd2, 0x07, 0x11, 0x0f, 0x1e, 0x1d, 0x75, 0xbc, 0xef, 0x55, 0x00, 0x00, 0x00, 0x09, 0x70, 0x48, 0x59, 0x73, 0x00, 0x00, 0x0b, 0x12, 0x00, 0x00, 0x0b, 0x12, 0x01, 0xd2, 0xdd, 0x7e, 0xfc, 0x00, 0x00, 0x00, 0x04, 0x67, 0x41, 0x4d, 0x41, 0x00, 0x00, 0xb1, 0x8f, 0x0b, 0xfc, 0x61, 0x05, 0x00, 0x00, 0x00, 0x06, 0x50, 0x4c, 0x54, 0x45, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x55, 0xc2, 0xd3, 0x7e, 0x00, 0x00, 0x00, 0x0a, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x60, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0xe5, 0x27, 0xde, 0xfc, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
   static const unsigned char glass[] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x01, 0x03, 0x00, 0x00, 0x00, 0x25, 0xdb, 0x56, 0xca, 0x00, 0x00, 0x00, 0x04, 0x67, 0x41, 0x4d, 0x41, 0x00, 0x00, 0xb1, 0x8f, 0x0b, 0xfc, 0x61, 0x05, 0x00, 0x00, 0x00, 0x06, 0x50, 0x4c, 0x54, 0x45, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x55, 0xc2, 0xd3, 0x7e, 0x00, 0x00, 0x00, 0x01, 0x74, 0x52, 0x4e, 0x53, 0x00, 0x40, 0xe6, 0xd8, 0x66, 0x00, 0x00, 0x00, 0x01, 0x62, 0x4b, 0x47, 0x44, 0x00, 0x88, 0x05, 0x1d, 0x48, 0x00, 0x00, 0x00, 0x09, 0x70, 0x48, 0x59, 0x73, 0x00, 0x00, 0x0b, 0x12, 0x00, 0x00, 0x0b, 0x12, 0x01, 0xd2, 0xdd, 0x7e, 0xfc, 0x00, 0x00, 0x00, 0x07, 0x74, 0x49, 0x4d, 0x45, 0x07, 0xd2, 0x07, 0x13, 0x0f, 0x08, 0x19, 0xc4, 0x40, 0x56, 0x10, 0x00, 0x00, 0x00, 0x0a, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x60, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0x48, 0xaf, 0xa4, 0x71, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

   writeBLOB(targetDir + "ruby.png", ruby, sizeof(ruby));
   writeBLOB(targetDir + "amber.png", amber, sizeof(amber));
   writeBLOB(targetDir + "emerald.png", emerald, sizeof(emerald));
   writeBLOB(targetDir + "snow.png", snow, sizeof(snow));
   writeBLOB(targetDir + "glass.png", glass, sizeof(glass));

   {
      string outputFile = targetDir + "llvmcov2html.css";
      ofstream out(outputFile);
      if (!out.is_open()) {
         cerr << "unable to write " << outputFile << endl;
         exit(1);
      }
      out << "/* Based upon the lcov CSS style, style files can be reused */" << endl
          << "body { color: #000000; background-color: #FFFFFF; }" << endl
          << "a:link { color: #284FA8; text-decoration: underline; }" << endl
          << "a:visited { color: #00CB40; text-decoration: underline; }" << endl
          << "a:active { color: #FF0040; text-decoration: underline; }" << endl
          << "td.title { text-align: center; padding-bottom: 10px; font-size: 20pt; font-weight: bold; }" << endl
          << "td.ruler { background-color: #6688D4; }" << endl
          << "td.headerItem { text-align: right; padding-right: 6px; font-family: sans-serif; font-weight: bold; }" << endl
          << "td.headerValue { text-align: left; color: #284FA8; font-family: sans-serif; font-weight: bold; }" << endl
          << "td.versionInfo { text-align: center; padding-top:  2px; }" << endl
          << "pre.source { font-family: monospace; white-space: pre; }" << endl
          << "span.lineNum { background-color: #EFE383; }" << endl
          << "span.lineCov { background-color: #CAD7FE; }" << endl
          << "span.linePartCov { background-color: #FFEA20; }" << endl
          << "span.lineNoCov { background-color: #FF6230; }" << endl
          << "td.tableHead { text-align: center; color: #FFFFFF; background-color: #6688D4; font-family: sans-serif; font-size: 120%; font-weight: bold; }" << endl
          << "td.coverFile { text-align: left; padding-left: 10px; padding-right: 20px; color: #284FA8; background-color: #DAE7FE; font-family: monospace; }" << endl
          << "td.coverBar { padding-left: 10px; padding-right: 10px; background-color: #DAE7FE; }" << endl
          << "td.coverBarOutline { background-color: #000000; }" << endl
          << "td.coverPerHi { text-align: right; padding-left: 10px; padding-right: 10px; background-color: #A7FC9D; font-weight: bold; }" << endl
          << "td.coverNumHi { text-align: right; padding-left: 10px; padding-right: 10px; background-color: #A7FC9D; }" << endl
          << "td.coverPerMed { text-align: right; padding-left: 10px; padding-right: 10px; background-color: #FFEA20; font-weight: bold; }" << endl
          << "td.coverNumMed { text-align: right; padding-left: 10px; padding-right: 10px; background-color: #FFEA20; }" << endl
          << "td.coverPerLo { text-align: right; padding-left: 10px; padding-right: 10px; background-color: #FF0000; font-weight: bold; }" << endl
          << "td.coverNumLo { text-align: right; padding-left: 10px; padding-right: 10px; background-color: #FF0000; }" << endl;
   }
}
//---------------------------------------------------------------------------
static string getFileTimestamp(const string& file)
// Get the timestamp of a file
{
   struct stat s;
   if (stat(file.c_str(), &s) != 0)
      return "";
   time_t t = s.st_mtime;
   return ctime(&t);
}
//---------------------------------------------------------------------------
int main(int argc, char** argv) {
   // Interpret the arguments
   string projectRoot;
   vector<string> extraIgnore;
   vector<string> ignoreDirs;

   bool hasProjectRoot = false;
   vector<string> args;
   for (int index = 1; index < argc; ++index) {
      if (argv[index][0] == '-') {
         string a = argv[index];
         if (a == "--") {
            for (++index; index != argc; ++index)
               args.push_back(argv[index]);
            break;
         } else if (a.substr(0, 14) == "--projectroot=") {
            projectRoot = a.substr(14);
            if ((!projectRoot.empty()) && (projectRoot.back() != '/'))
               projectRoot += '/';
            hasProjectRoot = true;
         } else if (a.substr(0, 15) == "--exclude-line=") {
            extraIgnore.push_back(a.substr(15));
         } else if (a.substr(0, 14) == "--exclude-dir=") {
            string dirs = a.substr(14);
            regex splitRegex(",");

            ignoreDirs = {
               sregex_token_iterator(dirs.begin(), dirs.end(), splitRegex, -1),
               sregex_token_iterator()};
         } else {
            cerr << "unknown option " << a << endl;
         }
      } else {
         args.push_back(argv[index]);
      }
   }
   if (args.size() != 3) {
      cerr << "usage: " << argv[0] << " targetDir executable default.prodata" << endl;
      return 1;
   }
   string targetDir = args[0];
   if ((!targetDir.empty()) && (targetDir.back() != '/'))
      targetDir += '/';

   // Load the coverage
   auto coverage = loadCoverage(args[1], args[2]);
   auto files = coverage->getUniqueSourceFiles();
   string binaryName = args[1];
   auto timestamp = getFileTimestamp(args[2]);

   // Compute the project root
   if (!hasProjectRoot) {
      if (!files.empty()) {
         string s = files.front().str();
         if (s.rfind('/') != string::npos)
            s = s.substr(0, s.rfind('/') + 1);
         for (auto& f : files) {
            auto c = f.str();
            while (c.substr(0, s.length()) != s) {
               if (s.length() < 2) {
                  s.clear();
                  break;
               }
               if (s.back() == '/')
                  s.resize(s.size() - 1);
               if (s.rfind('/') == string::npos) {
                  s.clear();
                  break;
               }
               s = s.substr(0, s.rfind('/') + 1);
            }
            if (s.empty())
               break;
         }
         if (!s.empty())
            projectRoot = s;
      }
   }

   // Translate all files
   struct FileInfo {
      string prettyName, htmlFile;
      unsigned hitLines, executableLines;
   };
   vector<FileInfo> fileInfo;
   CoverageList coverageList;
   for (auto& f : files) {
      string prettyName = f.str(), relName = "file";
      if ((!projectRoot.empty()) && (prettyName.substr(0, projectRoot.size()) == projectRoot)) {
         bool skip = false;

         if (!ignoreDirs.empty())
            for (const auto& ignoreDir : ignoreDirs)
               if (prettyName.substr(projectRoot.size(), ignoreDir.size()) == ignoreDir) {
                  skip = true;
                  break;
               }

         if (skip) continue;

         relName = prettyName.substr(projectRoot.size()) + ".html";
         prettyName = "[...]/" + prettyName.substr(projectRoot.size());
      }
      while (prettyName.find("/./") != string::npos) {
         auto split = prettyName.find("/./");
         prettyName = prettyName.substr(0, split) + prettyName.substr(split + 3);
      }

      replace(relName.begin(), relName.end(), '/', '_');
      string fileName = targetDir + relName;
      unsigned hitLines, executableLines;
      if (!processFile(coverageList, fileName, *coverage, f, extraIgnore, hitLines, executableLines, args[1], timestamp, prettyName))
         continue;

      fileInfo.push_back({prettyName, relName, hitLines, executableLines});
   }
   sort(fileInfo.begin(), fileInfo.end(), [](const FileInfo& a, const FileInfo& b) {
      unsigned perc1 = computePerc(a.hitLines, a.executableLines);
      unsigned perc2 = computePerc(b.hitLines, b.executableLines);
      if (perc1 != perc2)
         return perc1 < perc2;
      return a.prettyName < b.prettyName;
   });

   // Write the summary
   {
      ofstream out(targetDir + "index.html");
      unsigned hitLines = 0, executableLines = 0;
      for (auto& i : fileInfo) {
         hitLines += i.hitLines;
         executableLines += i.executableLines;
      }
      writeHeader(out, binaryName, timestamp, "", hitLines, executableLines);
      cout << "coverage: " << computePerc(hitLines, executableLines) / 10.0 << "%, " << (executableLines - hitLines) << " lines not reached" << endl;

      out << R"(<center>
                  <table width="80%" cellpadding="2" cellspacing="1" border="0">
                    <tr>
                      <td width="50%"><br/></td>
                      <td width="15%"></td>
                      <td width="15%"></td>
                      <td width="20%"></td>
                   </tr>
                 <tr>
                   <td class="tableHead">File</td>
                   <td class="tableHead" colspan="3">Coverage</td>
                 </tr>)"
          << endl;
      for (auto& i : fileInfo) {
         unsigned perc = computePerc(i.hitLines, i.executableLines);
         const char* qc = (perc >= 750) ? "Hi" : ((perc >= 350) ? "Med" : "Lo");
         out << R"(<tr>
                     <td class="coverFile"><a href=")"
             << i.htmlFile << "\">";
         escapeHtml(out, i.prettyName);
         out << R"(</a></td>
                     <td class="coverBar" align="center">
                       <table border="0" cellspacing="0" cellpadding="1"><tr><td class="coverBarOutline">)";
         constructBar(out, perc);
         out << R"(</td></tr></table>
                     </td>
                     <td class="coverPer)"
             << qc << "\">" << (perc / 10) << "." << (perc % 10) << R"(&nbsp;%</td>
                     <td class="coverNum)"
             << qc << "\">" << i.hitLines << "&nbsp;/&nbsp;" << i.executableLines << R"(&nbsp;lines</td>
                   </tr>)"
             << endl;
      }
      out << "  </table>" << endl
          << "</center>" << endl
          << "<br/>" << endl;

      writeFooter(out);
   }
   {
      ofstream out(targetDir + "hits");
      for (auto& c : coverageList)
         for (auto l : c.second.hits)
            out << c.first << ":" << l << endl;
   }
   {
      ofstream out(targetDir + "notreached");
      for (auto& c : coverageList)
         for (auto l : c.second.misses)
            out << c.first << ":" << l << endl;
   }

   // Write extra files
   writeExtras(targetDir);
}
//---------------------------------------------------------------------------
