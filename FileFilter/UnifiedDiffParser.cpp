// OpenCppCoverage is an open source code coverage for C++.
// Copyright (C) 2016 OpenCppCoverage
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "stdafx.h"
#include "UnifiedDiffParser.hpp"

#include <sstream>
#include <regex>
#include <filesystem>
#include <boost/algorithm/string.hpp>

#include "File.hpp"
#include "UnifiedDiffParserException.hpp"
#include "FileFilterException.hpp"
#include "Tools/Log.hpp"

namespace FileFilter
{
	namespace
	{
		const std::wstring GitTargetPrefix = L"b/";
		const std::wstring DevNull = L"/dev/null";

		//-----------------------------------------------------------------------
		bool IsGitDetected(
			const std::vector<File>& files,
			const std::vector<std::wstring>& sourceFileLines,
			bool foundGitHeader)
		{
			if (!foundGitHeader)
				return false;

			auto isGitTarget = std::all_of(files.begin(), files.end(), [](const auto& file)
			{
				return boost::algorithm::starts_with(file.GetPath().wstring(), GitTargetPrefix);
			});

			auto isGitSource = std::all_of(sourceFileLines.begin(), sourceFileLines.end(),
				[](const auto& line)
			{
				return boost::algorithm::starts_with(line, UnifiedDiffParser::FromFilePrefix + L"a/");
			});

			return isGitTarget && isGitSource;
		}

		//-----------------------------------------------------------------------
		void UpdateFilePathIfGitDetected(
			std::vector<File>& files,
			const std::vector<std::wstring>& sourceFileLines,
			bool foundGitHeader)
		{
			if (IsGitDetected(files, sourceFileLines, foundGitHeader))
			{
				LOG_INFO << "Diff file was generated by git diff.";
				for (auto& file : files)
				{
					auto path = file.GetPath().wstring();

					if (!boost::algorithm::starts_with(path, GitTargetPrefix))
						THROW(L"File should have the prefix: " + GitTargetPrefix);
					boost::algorithm::erase_head(path, static_cast<int>(GitTargetPrefix.size()));
					file.SetPath(path);
				}
			}
		}

		//---------------------------------------------------------------------
		template <typename Container, typename Fct>
		void EraseIf(Container& container, Fct fct)
		{
			auto it = std::remove_if(container.begin(), container.end(), fct);
			container.erase(it, container.end());
		}

		//---------------------------------------------------------------------
		void RemoveDevNull(
			std::vector<File>& files,
			std::vector<std::wstring>& sourceFileLines)
		{
			EraseIf(files, [](const auto& file)
			{
				return file.GetPath().wstring() == DevNull;
			});

			EraseIf(sourceFileLines, [](const auto& line)
			{
				return boost::algorithm::starts_with(line, UnifiedDiffParser::FromFilePrefix + DevNull);
			});
		}
	}
	//---------------------------------------------------------------------
	struct UnifiedDiffParser::HunksDifferences
	{
		int startFrom;
		int countFrom;
		int startTo;
		int countTo;
	};

	//---------------------------------------------------------------------
	struct UnifiedDiffParser::Stream
	{
		Stream(std::wistream& istr)
			: istr_{ istr }
			, currentLine_{ 0 }
			, lastLineRead_{ 0 }
		{
		}

		//---------------------------------------------------------------------
		std::wistream& GetLine(std::wstring& line)
		{
			std::getline(istr_, line);
			++currentLine_;
			lastLineRead_ = line;
			return istr_;
		}

		std::wistream& istr_;
		int currentLine_;
		std::wstring lastLineRead_;
	};

	//-------------------------------------------------------------------------
	const std::wstring UnifiedDiffParser::FromFilePrefix = L"--- ";
	const std::wstring UnifiedDiffParser::ToFilePrefix = L"+++ ";

	//-------------------------------------------------------------------------
	std::vector<File> UnifiedDiffParser::Parse(std::wistream& istr) const
	{
		std::wstring line;
		std::vector<File> files;
		Stream stream(istr);
		std::vector<std::wstring> sourceFileLines;
		bool foundGitHeader = false;

		while (stream.GetLine(line))
		{
			if (boost::algorithm::starts_with(line, L"diff --git"))
				foundGitHeader = true;
			else if (boost::algorithm::starts_with(line, FromFilePrefix))
			{
				std::wstring nextLine;
				if (stream.GetLine(nextLine))
				{
					if (boost::algorithm::starts_with(nextLine, ToFilePrefix))
					{
						sourceFileLines.push_back(line);
						files.emplace_back(ExtractTargetFile(nextLine));
					}
					else
						ThrowError(stream, UnifiedDiffParserException::ErrorExpectFromFilePrefix);
				}
				else
					ThrowError(stream, UnifiedDiffParserException::ErrorCannotReadLine);
			}
			else if (boost::algorithm::starts_with(line, L"@@"))
				FillUpdatedLines(line, files, stream);
		}
		RemoveDevNull(files, sourceFileLines);
		UpdateFilePathIfGitDetected(files, sourceFileLines, foundGitHeader);

		return files;
	}

	//---------------------------------------------------------------------
	void UnifiedDiffParser::FillUpdatedLines(
		const std::wstring& line,
		std::vector<File>& files,
		Stream& stream) const
	{
		if (files.empty())
			ThrowError(stream, UnifiedDiffParserException::ErrorNoFilenameBeforeHunks);
		auto updatedLines = ExtractUpdatedLines(stream, line);
		files.back().AddSelectedLines(updatedLines);
	}

	//---------------------------------------------------------------------
	std::filesystem::path UnifiedDiffParser::ExtractTargetFile(const std::wstring& line) const
	{
		const auto startIndex = ToFilePrefix.size();
		const auto endIndex = line.find('\t');
		if (endIndex != std::string::npos)
			return line.substr(startIndex, endIndex - startIndex);
		return line.substr(startIndex);
	}

	//-------------------------------------------------------------------------
	UnifiedDiffParser::HunksDifferences
		UnifiedDiffParser::ExtractHunksDifferences(
			const Stream& stream,
			const std::wstring& hunksDifferencesLine) const
	{
		std::wstring range = L"(\\d+)(?:,(\\d+))?";
		std::wregex hunkRegex{ L"^@@\\s*-" + range + L"\\s*\\+" + range + L"\\s*@@" };
		std::wcmatch match;

		if (std::regex_search(hunksDifferencesLine.c_str(), match, hunkRegex))
		{
			if (match.size() == 5 && match[1].matched && match[3].matched)
			{
				HunksDifferences hunksDifferences;
				hunksDifferences.startFrom = std::stoi(match[1]);
				hunksDifferences.countFrom = match[2].matched ? std::stoi(match[2]) : 1;
				hunksDifferences.startTo = std::stoi(match[3]);
				hunksDifferences.countTo = match[4].matched ? std::stoi(match[4]) : 1;

				return hunksDifferences;
			}
		}

		ThrowError(stream, UnifiedDiffParserException::ErrorInvalidHunks);
		return{};
	}

	//-------------------------------------------------------------------------
	std::vector<int> UnifiedDiffParser::ExtractUpdatedLines(
		Stream& stream,
		const std::wstring& hunksDifferencesLine) const
	{
		HunksDifferences hunksDifferences = ExtractHunksDifferences(stream, hunksDifferencesLine);

		std::wstring lineStr;
		int currentLine = hunksDifferences.startTo;
		const int endLine = hunksDifferences.startTo + hunksDifferences.countTo;
		std::vector<int> updatedLines;
		while (currentLine < endLine && stream.GetLine(lineStr))
		{
			if (!boost::algorithm::starts_with(lineStr, "-") &&
				!boost::algorithm::starts_with(lineStr, "\\")) // For: \ No newline at end of file
			{
				if (boost::algorithm::starts_with(lineStr, "+"))
					updatedLines.push_back(currentLine);
				++currentLine;
			}
		}

		if (currentLine != endLine)
			ThrowError(stream, UnifiedDiffParserException::ErrorContextHunks);
		return updatedLines;
	}

	//-------------------------------------------------------------------------
	void UnifiedDiffParser::ThrowError(const Stream& stream, const std::wstring& message) const
	{
		std::wostringstream ostr;

		ostr << L"Error line " << stream.currentLine_ << L": " << stream.lastLineRead_ << std::endl;
		ostr << message;
		throw UnifiedDiffParserException(ostr.str());
	}
}
