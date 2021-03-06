/* Session.cpp: session initialization

   Copyright (C) 1996-2018 Christian Schenk

   This file is part of the MiKTeX Core Library.

   The MiKTeX Core Library is free software; you can redistribute it
   and/or modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; either version 2, or
   (at your option) any later version.

   The MiKTeX Core Library is distributed in the hope that it will be
   useful, but WITHOUT ANY WARRANTY; without even the implied warranty
   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with the MiKTeX Core Library; if not, write to the Free
   Software Foundation, 59 Temple Place - Suite 330, Boston, MA
   02111-1307, USA. */

#include "config.h"

#if defined(MIKTEX_UNIX)
#  include <unistd.h>
#endif

#include <fstream>
#include <iostream>

// FIXME: must come first
#include "core-version.h"

#include <miktex/Core/ConfigNames>
#include <miktex/Core/Environment>
#include <miktex/Core/Paths>
#include <miktex/Core/Registry>
#include <miktex/Core/TemporaryDirectory>

#include "internal.h"

#include "Session/SessionImpl.h"

#if defined(MIKTEX_WINDOWS)
#  include "win/winRegistry.h"
#endif

using namespace std;

using namespace MiKTeX::Core;
using namespace MiKTeX::Trace;
using namespace MiKTeX::Util;

weak_ptr<SessionImpl> SessionImpl::theSession;

shared_ptr<Session> Session::Create(const Session::InitInfo& initInfo)
{
  MIKTEX_EXPECT(SessionImpl::theSession.expired());
  shared_ptr<SessionImpl> session = make_shared<SessionImpl>();
  SessionImpl::theSession = session;
  session->Initialize(initInfo);
  return session;
}

Session::~Session() noexcept
{
}

shared_ptr<Session> Session::Get()
{
  return SessionImpl::GetSession();
}

shared_ptr<Session> Session::TryGet()
{
  return SessionImpl::TryGetSession();
}

SessionImpl::SessionImpl() :
  // passing an empty string to the locale constructor is ok; it
  // means: "the user's preferred locale" (cf. "The C++ Programming
  // Language, Appendix D: Locales")
#if !defined(__MINGW32__)
  defaultLocale("")
#else
  // FIXME: work around MingW bug
  defaultLocale()
#endif
{
}

SessionImpl::~SessionImpl()
{
  try
  {
    Uninitialize();
  }
  catch (const MiKTeXException& ex)
  {
    try
    {
#if defined(MIKTEX_WINDOWS)
      ostringstream s;
      s << "error: ~Session(): " << ex;
      OutputDebugStringW(StringUtil::UTF8ToWideChar(s.str()).c_str());
#endif
    }
    catch (const exception&)
    {
    }
  }
  catch (const exception& ex)
  {
    try
    {
#if defined(MIKTEX_WINDOWS)
      ostringstream s;
      s << "error: ~Session(): " << ex.what();
      OutputDebugStringW(StringUtil::UTF8ToWideChar(s.str()).c_str());
#endif
    }
    catch (const exception&)
    {
    }
  }
}

void SessionImpl::Initialize(const Session::InitInfo& initInfo)
{
  string val;

  PathName programInvocationName = initInfo.GetProgramInvocationName();
  programInvocationName = programInvocationName.GetFileNameWithoutExtension();
  const char* lpsz = strstr(programInvocationName.GetData(), MIKTEX_ADMIN_SUFFIX);
  bool forceAdminMode = lpsz != nullptr && strlen(lpsz) == strlen(MIKTEX_ADMIN_SUFFIX);

#if defined(MIKTEX_WINDOWS)
  if (!forceAdminMode)
  {
    programInvocationName = GetMyProgramFile(false).TransformForComparison().GetFileNameWithoutExtension().GetData();
    lpsz = strstr(programInvocationName.GetData(), MIKTEX_ADMIN_SUFFIX);
    forceAdminMode = lpsz != nullptr && strlen(lpsz) == strlen(MIKTEX_ADMIN_SUFFIX);
  }
#endif

  adminMode = forceAdminMode || initInfo.GetOptions()[InitOption::AdminMode];

#if defined(MIKTEX_WINDOWS)
  if (initInfo.GetOptions()[InitOption::InitializeCOM])
  {
    MyCoInitialize();
  }
#endif

  initialized = true;

  this->initInfo = initInfo;

  theNameOfTheGame = initInfo.GetTheNameOfTheGame();

  RegisterLibraryTraceStreams();

  // enable trace streams
  string traceOptions;
  traceOptions = initInfo.GetTraceFlags();
  if (traceOptions.empty())
  {
    Utils::GetEnvironmentString(MIKTEX_ENV_TRACE, traceOptions);
  }
#if defined(MIKTEX_WINDOWS)
  if (traceOptions.empty() && (!initInfo.GetOptions()[InitOption::NoConfigFiles]))
  {
    if (!winRegistry::TryGetRegistryValue(TriState::False, MIKTEX_REGKEY_CORE, MIKTEX_REGVAL_TRACE, traceOptions))
    {
      traceOptions = "";
    }
  }
#endif
  if (!traceOptions.empty())
  {
    TraceStream::SetOptions(traceOptions);
  }

  DoStartupConfig();

  InitializeRootDirectories();

  Utils::GetEnvironmentString(MIKTEX_ENV_PACKAGE_LIST_FILE, packageHistoryFile);

  PushAppName(Utils::GetExeName());

  startDirectory.SetToCurrentDirectory();

  string miktexCwd;
  if (Utils::GetEnvironmentString(MIKTEX_ENV_CWD_LIST, miktexCwd))
  {
    for (const string& cwd : StringUtil::Split(miktexCwd, PathName::PathNameDelimiter))
    {
      AddInputDirectory(cwd, true);
    }
  }

  SetEnvironmentVariables();

  trace_core->WriteFormattedLine("core", T_("initializing MiKTeX Core version %s"), MIKTEX_COMPONENT_VERSION_STR);

#if defined(MIKTEX_WINDOWS) && defined(MIKTEX_CORE_SHARED)
  if (dynamicLoad == TriState::True)
  {
    trace_core->WriteFormattedLine("core", T_("dynamic load"));
  }
#endif

  trace_core->WriteFormattedLine("core", T_("operating system: %s"), Q_(Utils::GetOSVersionString()));
  trace_core->WriteFormattedLine("core", T_("program file: %s"), Q_(GetMyProgramFile(true)));
  trace_core->WriteFormattedLine("core", T_("current directory: %s"), Q_(PathName().SetToCurrentDirectory()));
  trace_config->WriteFormattedLine("core", T_("admin mode: %s"), IsAdminMode() ? T_("yes") : T_("no"));
  trace_config->WriteFormattedLine("core", T_("shared setup: %s"), IsSharedSetup() ? T_("yes") : T_("no"));

  trace_config->WriteFormattedLine("core", T_("session locale: %s"), Q_(defaultLocale.name()));

  if (IsAdminMode() && !IsSharedSetup())
  {
    MIKTEX_FATAL_ERROR(T_("Administrator mode startup refused because this is not a shared MiKTeX setup."));
  }

#if 1 // experimental
  if (RunningAsAdministrator() && IsSharedSetup() && !IsAdminMode())
  {
    if (GetConfigValue(MIKTEX_CONFIG_SECTION_CORE, MIKTEX_CONFIG_VALUE_AUTOADMIN).GetTriState() == TriState::True)
    {
      SetAdminMode(true, false);
    }
  }
#endif
}

void SessionImpl::Uninitialize()
{
  if (!initialized)
  {
    return;
  }
  try
  {
    StartFinishScript(10);
    initialized = false;
    trace_core->WriteFormattedLine("core", T_("uninitializing core library"));
    CheckOpenFiles();
    WritePackageHistory();
    inputDirectories.clear();
    UnregisterLibraryTraceStreams();
    configurationSettings.clear();
  }
  catch (const exception&)
  {
#if defined(MIKTEX_WINDOWS)
    while (numCoInitialize > 0)
    {
      MyCoUninitialize();
    }
#endif
    throw;
  }
#if defined(MIKTEX_WINDOWS)
  while (numCoInitialize > 0)
  {
    MyCoUninitialize();
  }
#endif
}

void SessionImpl::ScheduleSystemCommand(const std::string& commandLine)
{
  onFinishScript.push_back(commandLine);
}

void SessionImpl::StartFinishScript(int delay)
{
  if (onFinishScript.empty())
  {
    return;
  }
  trace_core->WriteFormattedLine("core", T_("finish script: %u commands to execute"), (unsigned)onFinishScript.size());
  unique_ptr<TemporaryDirectory> tmpdir = TemporaryDirectory::Create();
  trace_core->WriteFormattedLine("core", T_("finish script: tmpdir=%s"), tmpdir->GetPathName().GetData());
  vector<string> pre = {
#if defined(MIKTEX_WINDOWS)
    "ping localhost -n " + std::to_string(delay) + " >nul",
    "pushd "s + Q_(tmpdir->GetPathName().ToDos()),
#else
    "#!/bin/sh",
    "wait " + std::to_string(getpid()),
    "pushd "s + Q_(tmpdir->GetPathName()),
#endif
  };
  vector<string> post = {
#if defined(MIKTEX_WINDOWS)
    "popd",
#if !MIKTEX_KEEP_FINISH_SCRIPT
    "start \"\" /B cmd /C rmdir /S /Q "s + Q_(tmpdir->GetPathName().ToDos()),
#endif
#else
    "popd",
#if !MIKTEX_KEEP_FINISH_SCRIPT
    "rm -fr "s + Q_(tmpdir->GetPathName()),
#endif
#endif
  };
  PathName script = tmpdir->GetPathName() / GetMyProgramFile(false).GetFileNameWithoutExtension();
  script += "-finish";
#if defined(MIKTEX_WINDOWS)
  script.SetExtension(".cmd");
#endif
  ofstream writer = File::CreateOutputStream(script);
  for (const auto& cmd : pre)
  {
    writer << cmd << "\n";
  }
  for (const auto& cmd : onFinishScript)
  {
    writer << cmd << "\n";
  }
  for (const auto& cmd : post)
  {
    writer << cmd << "\n";
  }
  writer.close();
  trace_core->WriteFormattedLine("core", T_("starting finish script"));
#if defined(MIKTEX_UNIX)
  File::SetAttributes(script, { FileAttribute::Executable });
  Process::Start(script);
#else
  Process::StartSystemCommand(script.ToString());
#endif
  tmpdir->Keep();
}

void SessionImpl::Reset()
{
  vector<string> onFinishScript = move(this->onFinishScript);
  InitInfo initInfo = this->initInfo;
  this->~SessionImpl();
  new (this) SessionImpl();
  Initialize(initInfo);
  this->onFinishScript = move(onFinishScript);
}

void SessionImpl::SetEnvironmentVariables()
{
#if MIKTEX_WINDOWS
  // used in pdfcrop.pl
  Utils::SetEnvironmentString("TEXSYSTEM", "miktex");

  // Ghostscript
  Utils::SetEnvironmentString("GSC", MIKTEX_GS_EXE);
  vector<string> gsDirectories;
  gsDirectories.push_back((GetSpecialPath(SpecialPath::CommonInstallRoot) / "ghostscript" / "base").ToString());
  if (!IsAdminMode() && GetUserInstallRoot() != GetCommonInstallRoot())
  {
    gsDirectories.push_back((GetSpecialPath(SpecialPath::UserInstallRoot) / "ghostscript" / "base").ToString());
  }
  gsDirectories.push_back((GetSpecialPath(SpecialPath::CommonInstallRoot) / "fonts").ToString());
  if (!IsAdminMode() && GetUserInstallRoot() != GetCommonInstallRoot())
  {
    gsDirectories.push_back((GetSpecialPath(SpecialPath::UserInstallRoot) / "fonts").ToString());
  }
  Utils::SetEnvironmentString("MIKTEX_GS_LIB", StringUtil::Flatten(gsDirectories, PathName::PathNameDelimiter));
#endif

  PathName path = GetTempDirectory();

  if (!HaveEnvironmentString("TEMPDIR") || IsMiKTeXPortable())
  {
    Utils::SetEnvironmentString("TEMPDIR", path.ToString());
  }

  if (!HaveEnvironmentString("TMPDIR") || IsMiKTeXPortable())
  {
    Utils::SetEnvironmentString("TMPDIR", path.ToString());
  }

  if (!HaveEnvironmentString("TEMP") || IsMiKTeXPortable())
  {
    Utils::SetEnvironmentString("TEMP", path.ToString());
  }

  if (!HaveEnvironmentString("TMP") || IsMiKTeXPortable())
  {
    Utils::SetEnvironmentString("TMP", path.ToString());
  }

  if (!HaveEnvironmentString("HOME"))
  {
    Utils::SetEnvironmentString("HOME", GetHomeDirectory().ToString());
  }

  SetCWDEnv();

  if (!initInfo.GetOptions()[InitOption::NoFixPath])
  {
    string envPath;
    if (!GetEnvironmentString("PATH", envPath))
    {
      envPath = "";
    }
    string newEnvPath;
    bool competition;
    auto p = TryGetBinDirectory(true);
    if (p.first && FixProgramSearchPath(envPath, p.second, false, newEnvPath, competition))
    {
      Utils::SetEnvironmentString("PATH", newEnvPath);
      envPath = newEnvPath;
    }
#if !defined(MIKTEX_MACOS_BUNDLE)
    p = TryGetBinDirectory(false);
    if (p.first && FixProgramSearchPath(envPath, p.second, false, newEnvPath, competition))
    {
      Utils::SetEnvironmentString("PATH", newEnvPath);
      envPath = newEnvPath;
    }
#endif
  }
}

void SessionImpl::SetTheNameOfTheGame(const string& name)
{
  fileTypes.clear();
  theNameOfTheGame = name;
}

