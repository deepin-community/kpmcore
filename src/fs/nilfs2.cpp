/*
    SPDX-FileCopyrightText: 2012-2018 Andrius Štikonas <andrius@stikonas.eu>
    SPDX-FileCopyrightText: 2019 Yuri Chornoivan <yurchor@ukr.net>
    SPDX-FileCopyrightText: 2020 Arnaud Ferraris <arnaud.ferraris@collabora.com>
    SPDX-FileCopyrightText: 2020 Gaël PORTAY <gael.portay@collabora.com>

    SPDX-License-Identifier: GPL-3.0-or-later
*/

#include "fs/nilfs2.h"

#include "util/externalcommand.h"
#include "util/capacity.h"
#include "util/report.h"

#include <cmath>

#include <QRegularExpression>
#include <QString>
#include <QTemporaryDir>
#include <QUuid>

#include <KLocalizedString>

namespace FS
{
FileSystem::CommandSupportType nilfs2::m_GetUsed = FileSystem::cmdSupportNone;
FileSystem::CommandSupportType nilfs2::m_GetLabel = FileSystem::cmdSupportNone;
FileSystem::CommandSupportType nilfs2::m_Create = FileSystem::cmdSupportNone;
FileSystem::CommandSupportType nilfs2::m_Grow = FileSystem::cmdSupportNone;
FileSystem::CommandSupportType nilfs2::m_Shrink = FileSystem::cmdSupportNone;
FileSystem::CommandSupportType nilfs2::m_Move = FileSystem::cmdSupportNone;
FileSystem::CommandSupportType nilfs2::m_Check = FileSystem::cmdSupportNone;
FileSystem::CommandSupportType nilfs2::m_Copy = FileSystem::cmdSupportNone;
FileSystem::CommandSupportType nilfs2::m_Backup = FileSystem::cmdSupportNone;
FileSystem::CommandSupportType nilfs2::m_SetLabel = FileSystem::cmdSupportNone;
FileSystem::CommandSupportType nilfs2::m_UpdateUUID = FileSystem::cmdSupportNone;
FileSystem::CommandSupportType nilfs2::m_GetUUID = FileSystem::cmdSupportNone;

nilfs2::nilfs2(qint64 firstsector, qint64 lastsector, qint64 sectorsused, const QString& label, const QVariantMap& features) :
    FileSystem(firstsector, lastsector, sectorsused, label, features, FileSystem::Type::Nilfs2)
{
}

void nilfs2::init()
{
    m_Create = findExternal(QStringLiteral("mkfs.nilfs2")) ? cmdSupportFileSystem : cmdSupportNone;
    m_Check = /*findExternal(QStringLiteral("fsck.nilfs2")) ? cmdSupportFileSystem : */cmdSupportNone;

    m_GetLabel = cmdSupportCore;
    m_SetLabel = findExternal(QStringLiteral("nilfs-tune")) ? cmdSupportFileSystem : cmdSupportNone;
    m_UpdateUUID = findExternal(QStringLiteral("nilfs-tune")) ? cmdSupportFileSystem : cmdSupportNone;

    m_Grow = findExternal(QStringLiteral("nilfs-resize")) ? cmdSupportFileSystem : cmdSupportNone;
    m_GetUsed = findExternal(QStringLiteral("nilfs-tune")) ? cmdSupportFileSystem : cmdSupportNone;
    m_Shrink = (m_Grow != cmdSupportNone && m_GetUsed != cmdSupportNone) ? cmdSupportFileSystem : cmdSupportNone;

    m_Copy =/* (m_Check != cmdSupportNone) ?*/ cmdSupportCore /*: cmdSupportNone*/;
    m_Move =/* (m_Check != cmdSupportNone) ?*/ cmdSupportCore /*: cmdSupportNone*/;

    m_GetLabel = cmdSupportCore;
    m_Backup = cmdSupportCore;
    m_GetUUID = cmdSupportCore;
}

bool nilfs2::supportToolFound() const
{
    return
        m_GetUsed != cmdSupportNone &&
        m_GetLabel != cmdSupportNone &&
        m_SetLabel != cmdSupportNone &&
        m_Create != cmdSupportNone &&
//         m_Check != cmdSupportNone &&
        m_UpdateUUID != cmdSupportNone &&
        m_Grow != cmdSupportNone &&
        m_Shrink != cmdSupportNone &&
        m_Copy != cmdSupportNone &&
        m_Move != cmdSupportNone &&
        m_Backup != cmdSupportNone &&
        m_GetUUID != cmdSupportNone;
}

FileSystem::SupportTool nilfs2::supportToolName() const
{
    return SupportTool(QStringLiteral("nilfs2-utils"), QUrl(QStringLiteral("https://github.com/nilfs-dev/nilfs-utils")));
}

qint64 nilfs2::minCapacity() const
{
    return 128 * Capacity::unitFactor(Capacity::Unit::Byte, Capacity::Unit::MiB);
}

qint64 nilfs2::maxCapacity() const
{
    return Capacity::unitFactor(Capacity::Unit::Byte, Capacity::Unit::EiB);
}

int nilfs2::maxLabelLength() const
{
    return 80;
}

bool nilfs2::check(Report& report, const QString& deviceNode) const
{
    ExternalCommand cmd(report, QStringLiteral("fsck.nilfs2"), { deviceNode });
    return cmd.run(-1) && cmd.exitCode() == 0;
}

bool nilfs2::create(Report& report, const QString& deviceNode)
{
    ExternalCommand cmd(report, QStringLiteral("mkfs.nilfs2"), { QStringLiteral("-f"), deviceNode });
    return cmd.run(-1) && cmd.exitCode() == 0;
}

qint64 nilfs2::readUsedCapacity(const QString& deviceNode) const
{
    ExternalCommand cmd(QStringLiteral("nilfs-tune"), { QStringLiteral("-l"), deviceNode });

    if (cmd.run(-1) && cmd.exitCode() == 0) {
        QRegularExpression re(QStringLiteral("Block size:\\s+(\\d+)"));
        QRegularExpressionMatch reBlockSize = re.match(cmd.output());
        re.setPattern(QStringLiteral("Device size:\\s+(\\d+)"));
        QRegularExpressionMatch reDeviceSize = re.match(cmd.output());
        re.setPattern(QStringLiteral("Free blocks count:\\s+(\\d+)"));
        QRegularExpressionMatch reFreeBlocks = re.match(cmd.output());
        if (reBlockSize.hasMatch() && reDeviceSize.hasMatch() && reFreeBlocks.hasMatch())
            return reDeviceSize.captured(1).toLongLong() - reBlockSize.captured(1).toLongLong() * reFreeBlocks.captured(1).toLongLong();
    }

    return -1;
}

bool nilfs2::resize(Report& report, const QString& deviceNode, qint64 length) const
{
    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        report.line() << xi18nc("@info:progress", "Resizing NILFS2 file system on partition <filename>%1</filename> failed: Could not create temp dir.", deviceNode);
        return false;
    }

    bool rval = false;

    ExternalCommand mountCmd(report, QStringLiteral("mount"), { QStringLiteral("--verbose"), QStringLiteral("--types"), QStringLiteral("nilfs2"), deviceNode, tempDir.path() });

    if (mountCmd.run(-1) && mountCmd.exitCode() == 0) {
        ExternalCommand resizeCmd(report, QStringLiteral("nilfs-resize"), { QStringLiteral("--verbose"), QStringLiteral("--assume-yes"), deviceNode, QString::number(length) });

        if (resizeCmd.run(-1) && resizeCmd.exitCode() == 0)
            rval = true;
        else
            report.line() << xi18nc("@info:progress", "Resizing NILFS2 file system on partition <filename>%1</filename> failed: NILFS2 file system resize failed.", deviceNode);

        ExternalCommand unmountCmd(report, QStringLiteral("umount"), { tempDir.path() });

        if (!unmountCmd.run(-1) && unmountCmd.exitCode() == 0)
            report.line() << xi18nc("@info:progress", "<warning>Resizing NILFS2 file system on partition <filename>%1</filename>: Unmount failed.</warning>", deviceNode);
    } else
        report.line() << xi18nc("@info:progress", "Resizing NILFS2 file system on partition <filename>%1</filename> failed: Initial mount failed.", deviceNode);

    return rval;
}

bool nilfs2::resizeOnline(Report& report, const QString& deviceNode, const QString&, qint64 length) const
{
    ExternalCommand resizeCmd(report, QStringLiteral("nilfs-resize"), { QStringLiteral("--verbose"), QStringLiteral("--assume-yes"), deviceNode, QString::number(length) });

    if (resizeCmd.run(-1) && resizeCmd.exitCode() == 0)
        return true;

    report.line() << xi18nc("@info:progress", "Resizing NILFS2 file system on partition <filename>%1</filename> failed: NILFS2 file system resize failed.", deviceNode);
    return false;
}

bool nilfs2::writeLabel(Report& report, const QString& deviceNode, const QString& newLabel)
{
    ExternalCommand cmd(report, QStringLiteral("nilfs-tune"), { QStringLiteral("-l"), newLabel, deviceNode });
    return cmd.run(-1) && cmd.exitCode() == 0;
}

bool nilfs2::updateUUID(Report& report, const QString& deviceNode) const
{
    QUuid uuid = QUuid::createUuid();
    ExternalCommand cmd(report, QStringLiteral("nilfs-tune"), { QStringLiteral("-U"), uuid.toString(), deviceNode });
    return cmd.run(-1) && cmd.exitCode() == 0;
}
}
