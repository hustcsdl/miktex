/* PackageProxyModel.cpp:

   Copyright (C) 2017 Christian Schenk

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

#include <miktex/Core/PathName>

#include "PackageProxyModel.h"

using namespace MiKTeX::Core;

PackageProxyModel::PackageProxyModel(QObject* parent) :
  QSortFilterProxyModel(parent)
{
}

bool PackageProxyModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const
{
  if (!fileNamePattern.empty())
  {
    QModelIndex index6 = sourceModel()->index(sourceRow, 6, sourceParent);
    QList<QVariant> runFiles = sourceModel()->data(index6, Qt::UserRole).toList();
    bool found = false;
    for (const QVariant& v : runFiles)
    {
      found = PathName::Match(fileNamePattern.c_str(), PathName(v.toString().toUtf8().constData()).RemoveDirectorySpec());
      if (found)
      {
        break;
      }
    }
    return found;
  }
  return true;
}