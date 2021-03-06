/* PackageDataStore.cpp

   Copyright (C) 2018 Christian Schenk

   This file is part of MiKTeX Package Manager.

   MiKTeX Package Manager is free software; you can redistribute it
   and/or modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; either version 2, or
   (at your option) any later version.

   MiKTeX Package Manager is distributed in the hope that it will be
   useful, but WITHOUT ANY WARRANTY; without even the implied warranty
   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with MiKTeX Package Manager; if not, write to the Free
   Software Foundation, 59 Temple Place - Suite 330, Boston, MA
   02111-1307, USA. */

#include "config.h"

#include <future>

#include <fmt/format.h>
#include <fmt/ostream.h>

#include <miktex/Core/Directory>
#include <miktex/Core/DirectoryLister>
#include <miktex/Trace/StopWatch>
#include <miktex/Trace/Trace>
#include <miktex/Trace/TraceStream>
#include <miktex/Util/StringUtil>

#include "internal.h"

#include "PackageDataStore.h"
#include "TpmParser.h"

using namespace std;

using namespace MiKTeX::Core;
using namespace MiKTeX::Packages;
using namespace MiKTeX::Trace;
using namespace MiKTeX::Util;

using namespace MiKTeX::Packages::D6AAD62216146D44B580E92711724B78;

PackageDataStore::PackageDataStore() :
  // TODO: trace callback
  trace_mpm(TraceStream::Open(MIKTEX_TRACE_MPM)),
  trace_stopwatch(TraceStream::Open(MIKTEX_TRACE_STOPWATCH))
{
}

void PackageDataStore::LoadAllPackageManifests(const PathName& packageManifestsPath)
{
  trace_mpm->WriteLine(TRACE_FACILITY, fmt::format(T_("loading all package manifests ({0})"), Q_(packageManifestsPath)));

  if (!File::Exists(packageManifestsPath))
  {
    trace_mpm->WriteLine(TRACE_FACILITY, fmt::format(T_("file {0} does not exist"), Q_(packageManifestsPath)));
    return;
  }

  unique_ptr<Cfg> cfg = Cfg::Create();
  cfg->Read(packageManifestsPath);

  Load(*cfg);

  loadedAllPackageManifests = true;
}

void PackageDataStore::SaveVarData()
{
  comboCfg.Save();
}

void PackageDataStore::Clear()
{
  packageTable.clear();
  installedFileInfoTable.clear();
  loadedAllPackageManifests = false;
  comboCfg.Clear();
}

tuple<bool, PackageInfo> PackageDataStore::TryGetPackage(const string& packageId)
{
  Load();
  auto it = packageTable.find(packageId);
  if (it == packageTable.end())
  {
    return make_tuple(false, PackageInfo());
  }
  else
  {
    return make_tuple(true, it->second);
  }
}

void PackageDataStore::SetTimeInstalled(const string& packageId, time_t timeInstalled)
{
  (*this)[packageId].timeInstalled = timeInstalled;
  if (session->IsAdminMode())
  {
    (*this)[packageId].timeInstalledByAdmin = timeInstalled;
  }
  else
  {
    (*this)[packageId].timeInstalledByUser = timeInstalled;
  }
  if (IsValidTimeT(timeInstalled))
  {
    comboCfg.PutValue(packageId, "TimeInstalled", std::to_string(timeInstalled));
  }
  else
  {
    comboCfg.DeleteKey(packageId);
  }
}

void PackageDataStore::DeclareObsolete(const string& packageId)
{
  (*this)[packageId].isObsolete = true;
  comboCfg.PutValue(packageId, "Obsolete", "1");
}

void PackageDataStore::SetReleaseState(const string& packageId, RepositoryReleaseState releaseState)
{
  (*this)[packageId].releaseState = releaseState;
  comboCfg.PutValue(packageId, "ReleaseState", releaseState == RepositoryReleaseState::Next ? "next" : releaseState == RepositoryReleaseState::Stable ? "stable" : "");
}

PackageDataStore::iterator PackageDataStore::begin()
{
  Load();
  return iterator(packageTable.begin());
}

PackageDataStore::iterator PackageDataStore::end()
{
  Load();
  return iterator(packageTable.end());
}

void PackageDataStore::DefinePackage(const PackageInfo& packageInfo)
{
  pair<PackageDefinitionTable::iterator, bool> p = packageTable.insert(make_pair(packageInfo.id, packageInfo));
  if (session->IsMiKTeXDirect())
  {
    // installed from the start
    p.first->second.isRemovable = false;
    p.first->second.isObsolete = false;
    p.first->second.timeInstalled = packageInfo.timePackaged;
    p.first->second.timeInstalledByAdmin = packageInfo.timePackaged;
    p.first->second.timeInstalledByUser = packageInfo.timePackaged;
  }
  else
  {
    p.first->second.isRemovable = IsRemovable(p.first->second.id);
    p.first->second.isObsolete = IsObsolete(p.first->second.id);
    p.first->second.timeInstalled = GetTimeInstalled(p.first->second.id);
    p.first->second.timeInstalledByAdmin = GetCommonTimeInstalled(p.first->second.id);
    p.first->second.timeInstalledByUser = GetUserTimeInstalled(p.first->second.id);
    if (p.first->second.IsInstalled())
    {
      p.first->second.releaseState = GetReleaseState(p.first->second.id);
    }
  }
}

void PackageDataStore::IncrementFileRefCounts(const string& packageId)
{
  const PackageInfo& package = (*this)[packageId];
  IncrementFileRefCounts(package.runFiles);
  IncrementFileRefCounts(package.docFiles);
  IncrementFileRefCounts(package.sourceFiles);
}

unsigned long PackageDataStore::GetFileRefCount(const PathName& path)
{
  Load();
  InstalledFileInfoTable::const_iterator it = installedFileInfoTable.find(path.ToString());
  if (it == installedFileInfoTable.end())
  {
    return 0;
  }
  return it->second.refCount;
}

unsigned long PackageDataStore::DecrementFileRefCount(const PathName& path)
{
  Load();
  InstalledFileInfoTable::iterator it = installedFileInfoTable.find(path.ToString());
  if (it == installedFileInfoTable.end() || it->second.refCount == 0)
  {
    MIKTEX_UNEXPECTED();
  }
  it->second.refCount--;
  return it->second.refCount;
}

void PackageDataStore::NeedPackageManifestsIni()
{
  PathName existingPackageManifestsIni = session->GetSpecialPath(SpecialPath::InstallRoot) / MIKTEX_PATH_PACKAGE_MANIFESTS_INI;
  if (File::Exists(existingPackageManifestsIni))
  {
    return;
  }
  PathName tpmDir = session->GetSpecialPath(SpecialPath::InstallRoot) / MIKTEX_PATH_PACKAGE_MANIFEST_DIR;
  if (Directory::Exists(tpmDir))
  {
    trace_mpm->WriteLine(TRACE_FACILITY, fmt::format("starting migration: {} -> {}", tpmDir, existingPackageManifestsIni));
    unique_ptr<Cfg> cfgExisting = Cfg::Create();
    unique_ptr<DirectoryLister> lister = DirectoryLister::Open(tpmDir);
    DirectoryEntry direntry;
    unique_ptr<TpmParser> tpmParser = TpmParser::Create();
    int count = 0;
    while (lister->GetNext(direntry))
    {
      PathName name(direntry.name);
      if (direntry.isDirectory || !name.HasExtension(MIKTEX_PACKAGE_MANIFEST_FILE_SUFFIX))
      {
        continue;
      }
      tpmParser->Parse(tpmDir / name);
      PackageInfo packageInfo = tpmParser->GetPackageInfo();
      PackageManager::PutPackageManifest(*cfgExisting, packageInfo, packageInfo.timePackaged);
      count++;
    }
    cfgExisting->Write(existingPackageManifestsIni);
    trace_mpm->WriteLine(TRACE_FACILITY, fmt::format("successfully migrated {} package manifest files", count));
  }
}

void PackageDataStore::Load()
{
  if (loadedAllPackageManifests)
  {
    // we do this once
    return;
  }
  unique_ptr<StopWatch> stopWatch = StopWatch::Start(trace_stopwatch.get(), TRACE_FACILITY, "loading all package manifests");
  NeedPackageManifestsIni();
  unique_ptr<Cfg> cfg = Cfg::Create();
  if (!session->IsAdminMode())
  {
    PathName userPath = session->GetSpecialPath(SpecialPath::UserInstallRoot) / MIKTEX_PATH_PACKAGE_MANIFESTS_INI;
    if (File::Exists(userPath))
    {
      cfg->Read(userPath);
    }
  }
  PathName commonPath = session->GetSpecialPath(SpecialPath::CommonInstallRoot) / MIKTEX_PATH_PACKAGE_MANIFESTS_INI;
  if ((session->IsAdminMode() || session->GetSpecialPath(SpecialPath::UserInstallRoot).Canonicalize() != session->GetSpecialPath(SpecialPath::CommonInstallRoot).Canonicalize()) && File::Exists(commonPath))
  {
    cfg->SetOptions({ Cfg::Option::NoOverwriteKeys });
    cfg->Read(commonPath);
  }
  Load(*cfg);
  loadedAllPackageManifests = true;
}

void PackageDataStore::Load(Cfg& cfg)
{
  unsigned count = 0;
  for (const auto& key : cfg)
  {
    // ignore redefinition
    if (packageTable.find(key->GetName()) != packageTable.end())
    {
      continue;
    }

    PackageInfo packageInfo = PackageManager::GetPackageManifest(cfg, key->GetName(), TEXMF_PREFIX_DIRECTORY);

#if IGNORE_OTHER_SYSTEMS
    string targetSystems = packageInfo.targetSystem;
    if (targetSystems != "" && !StringUtil::Contains(targetSystems.c_str(), MIKTEX_SYSTEM_TAG))
    {
      trace_mpm->WriteLine(TRACE_FACILITY, fmt::format(T_("{0}: ignoring {1} package"), packageInfo.id, targetSystems));
      continue;
    }
#endif

    count += 1;

    // insert into database
    DefinePackage(packageInfo);

    // increment file ref counts, if package is installed
    if (packageInfo.IsInstalled())
    {
      IncrementFileRefCounts(packageInfo.runFiles);
      IncrementFileRefCounts(packageInfo.docFiles);
      IncrementFileRefCounts(packageInfo.sourceFiles);
    }
  }

  trace_mpm->WriteLine(TRACE_FACILITY, fmt::format(T_("found {0} package manifests"), count));

  // determine dependencies
  for (auto& kv : packageTable)
  {
    PackageInfo& pkg = kv.second;
    // FIXME
    time_t timeInstalledMin = static_cast<time_t>(0xffffffffffffffffULL);
    time_t timeInstalledMax = 0;
    for (const string& req : pkg.requiredPackages)
    {
      PackageDefinitionTable::iterator it3 = packageTable.find(req);
      if (it3 == packageTable.end())
      {
        trace_mpm->WriteLine(TRACE_FACILITY, fmt::format(T_("dependancy problem: {0} is required by {1}"), req, pkg.id));
      }
      else
      {
        it3->second.requiredBy.push_back(pkg.id);
        if (it3->second.timeInstalled < timeInstalledMin)
        {
          timeInstalledMin = it3->second.timeInstalled;
        }
        if (it3->second.timeInstalled > timeInstalledMax)
        {
          timeInstalledMax = it3->second.timeInstalled;
        }
      }
    }
    if (timeInstalledMin > 0)
    {
      if (pkg.IsPureContainer() || (pkg.IsInstalled() && pkg.timeInstalled < timeInstalledMax))
      {
        pkg.timeInstalled = timeInstalledMax;
      }
    }
  }

  // create "Obsolete" container
  PackageInfo piObsolete;
  piObsolete.id = "_miktex-obsolete";
  piObsolete.displayName = T_("Obsolete");
  piObsolete.title = T_("Obsolete packages");
  piObsolete.description = T_("Packages that were removed from the MiKTeX package repository.");
  for (auto& kv : packageTable)
  {
    PackageInfo& pkg = kv.second;
    if (!pkg.IsContained() && !pkg.IsContainer() && IsObsolete(pkg.id))
    {
      piObsolete.requiredPackages.push_back(pkg.id);
      pkg.requiredBy.push_back(piObsolete.id);
    }
  }
  if (!piObsolete.requiredPackages.empty())
  {
    // insert "Obsolete" into the database
    DefinePackage(piObsolete);
  }

  // create "Uncategorized" container
  PackageInfo piOther;
  piOther.id = "_miktex-all-the-rest";
  piOther.displayName = T_("Uncategorized");
  piOther.title = T_("Uncategorized packages");
  for (auto& kv : packageTable)
  {
    PackageInfo& pkg = kv.second;
    if (!pkg.IsContained() && !pkg.IsContainer())
    {
      piOther.requiredPackages.push_back(pkg.id);
      pkg.requiredBy.push_back(piOther.id);
    }
  }
  if (!piOther.requiredPackages.empty())
  {
    // insert "Other" into the database
    DefinePackage(piOther);
  }
}

void PackageDataStore::LoadVarData()
{
  if (!comboCfg.Loaded())
  {
    comboCfg.Load(
      session->IsAdminMode() ? "" : session->GetSpecialPath(SpecialPath::UserInstallRoot) / MIKTEX_PATH_PACKAGES_INI,
      session->GetSpecialPath(SpecialPath::CommonInstallRoot) / MIKTEX_PATH_PACKAGES_INI);
  }
}

PackageInfo& PackageDataStore::operator[](const string& packageId)
{
  Load();
  auto it = packageTable.find(packageId);
  if (it == packageTable.end())
  {
    MIKTEX_FATAL_ERROR_2(T_("The requested package is unknown."), "name", packageId);
  }
  return it->second;
}

time_t PackageDataStore::GetUserTimeInstalled(const string& packageId)
{
  LoadVarData();
  string str;
  if (comboCfg.TryGetValueAsString(ComboCfg::Scope::User, packageId, "TimeInstalled", str))
  {
    return Utils::ToTimeT(str);
  }
  else
  {
    return InvalidTimeT;
  }
}

time_t PackageDataStore::GetCommonTimeInstalled(const std::string& packageId)
{
  LoadVarData();
  string str;
  if (comboCfg.TryGetValueAsString(ComboCfg::Scope::Common, packageId, "TimeInstalled", str))
  {
    return Utils::ToTimeT(str);
  }
  else
  {
    return InvalidTimeT;
  }
}

time_t PackageDataStore::GetTimeInstalled(const string& packageId)
{
  LoadVarData();
  string str;
  if ((!session->IsAdminMode() && comboCfg.TryGetValueAsString(ComboCfg::Scope::User, packageId, "TimeInstalled", str))
    || comboCfg.TryGetValueAsString(ComboCfg::Scope::Common, packageId, "TimeInstalled", str))
  {
    return Utils::ToTimeT(str);
  }
  else
  {
    return InvalidTimeT;
  }
}

bool PackageDataStore::IsRemovable(const string& packageId)
{
  LoadVarData();
  bool ret;
  string str;
  if (session->IsAdminMode())
  {
    // administrator can remove system-wide packages
    ret = IsValidTimeT(GetCommonTimeInstalled(packageId));
  }
  else
  {
    // user can remove private packages
    if (session->GetSpecialPath(SpecialPath::CommonInstallRoot).Canonicalize() == session->GetSpecialPath(SpecialPath::UserInstallRoot).Canonicalize())
    {
      ret = IsValidTimeT(GetTimeInstalled(packageId));
    }
    else
    {
      ret = IsValidTimeT(GetUserTimeInstalled(packageId));
    }
  }
  return ret;
}

bool PackageDataStore::IsObsolete(const string& packageId)
{
  LoadVarData();
  string str;
  if ((!session->IsAdminMode() && comboCfg.TryGetValueAsString(ComboCfg::Scope::User, packageId, "Obsolete", str))
    || comboCfg.TryGetValueAsString(ComboCfg::Scope::Common, packageId, "Obsolete", str))
  {
    return std::stoi(str) != 0;
  }
  else
  {
    return false;
  }
}

RepositoryReleaseState PackageDataStore::GetReleaseState(const string& packageId)
{
  LoadVarData();
  string str;
  if (comboCfg.TryGetValueAsString(packageId, "ReleaseState", str))
  {
    if (str == "stable")
    {
      return RepositoryReleaseState::Stable;
    }
    else if (str == "next")
    {
      return RepositoryReleaseState::Next;
    }
  }
  return RepositoryReleaseState::Unknown;
}

void PackageDataStore::IncrementFileRefCounts(const vector<string>& files)
{
  for (const string& file : files)
  {
    ++installedFileInfoTable[file].refCount;
#if POLLUTE_THE_DEBUG_STREAM
    if (installedFileInfoTable[file].refCount >= 2)
    {
      trace_mpm->WriteLine(TRACE_FACILITY, fmt::format(T_("{0}: ref count > 1"), Q_(file)));
    }
#endif
  }
}

size_t PackageDataStore::GetNumberOfInstalledPackages(bool userScope)
{
  LoadVarData();
  return comboCfg.GetSize(userScope ? ComboCfg::Scope::User : ComboCfg::Scope::Common);
}
