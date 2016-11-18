/*
 * Copyright (C) 2016 Jolla Ltd
 * Contact: Andrew den Exter <andrew.den.exter@jolla.com>
 *
 * You may use this file under the terms of the BSD license as follows:
 *
 * "Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Nemo Mobile nor the names of its contributors
 *     may be used to endorse or promote products derived from this
 *     software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE."
 */

#include "settingswatcher.h"

#include <QDebug>
#include <QEvent>
#include <QFile>
#include <QSettings>

#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <unistd.h>

const char * const SettingsWatcher::automaticLockingKey = "/desktop/nemo/devicelock/automatic_locking";
const char * const SettingsWatcher::minimumLengthKey = "/desktop/nemo/devicelock/code_min_length";
const char * const SettingsWatcher::maximumLengthKey = "/desktop/nemo/devicelock/code_max_length";
const char * const SettingsWatcher::maximumAttemptsKey = "/desktop/nemo/devicelock/maximum_attempts";
const char * const SettingsWatcher::peekingAllowedKey = "/desktop/nemo/devicelock/peeking_allowed";
const char * const SettingsWatcher::sideloadingAllowedKey = "/desktop/nemo/devicelock/sideloading_allowed";
const char * const SettingsWatcher::showNotificationsKey = "/desktop/nemo/devicelock/show_notification";
const char * const SettingsWatcher::inputIsKeyboardKey = "/desktop/nemo/devicelock/code_input_is_keyboard";
const char * const SettingsWatcher::currentIsDigitOnlyKey = "/desktop/nemo/devicelock/code_current_is_digit_only";

SettingsWatcher *SettingsWatcher::sharedInstance = nullptr;

SettingsWatcher::SettingsWatcher(QObject *parent)
    : QSocketNotifier(inotify_init(), Read, parent)
    , automaticLocking(10)
    , minimumLength(5)
    , maximumLength(42)
    , maximumAttempts(-1)
    , peekingAllowed(1)
    , sideloadingAllowed(-1)
    , showNotifications(1)
    , inputIsKeyboard(false)
    , currentCodeIsDigitOnly(true)
    , m_settingsPath(QStringLiteral("/usr/share/lipstick/devicelock/devicelock_settings.conf"))
    , m_watch(-1)
{
    Q_ASSERT(!sharedInstance);
    sharedInstance = this;

    watchForSettingsFile();
}

SettingsWatcher::~SettingsWatcher()
{
    close(socket());

    sharedInstance = nullptr;
}

SettingsWatcher *SettingsWatcher::instance()
{
    if (sharedInstance)
        return sharedInstance;

    return sharedInstance ? sharedInstance : new SettingsWatcher;
}

bool SettingsWatcher::event(QEvent *event)
{
    if (event->type() == QEvent::SockAct) {
        int bufferSize = 0;
        ioctl(socket(), FIONREAD, (char *) &bufferSize);
        QVarLengthArray<char, 4096> buffer(bufferSize);

        bufferSize = read(socket(), buffer.data(), bufferSize);
        char *at = buffer.data();
        char * const end = at + bufferSize;

        struct inotify_event *pevent = 0;
        for (;at < end; at += sizeof(inotify_event) + pevent->len) {
            pevent = reinterpret_cast<inotify_event *>(at);

            if (pevent->wd != m_watch) {
                continue;
            } else if (pevent->mask & IN_CREATE) {
                if (QFile::exists(m_settingsPath)) {
                    const auto watch = m_watch;

                    watchSettingsFile();

                    inotify_rm_watch(socket(), watch);
                }
            } else if (pevent->mask & IN_DELETE_SELF) {
                const auto watch = m_watch;

                watchForSettingsFile();

                inotify_rm_watch(socket(), watch);
            } else if (pevent->mask & IN_CLOSE_WRITE) {
                reloadSettings();
            }
        }

        return true;
    } else {
        return QSocketNotifier::event(event);
    }
}

void SettingsWatcher::watchForSettingsFile()
{
    if (QFile::exists(m_settingsPath)) {
        watchSettingsFile();
    } else {
        m_watch = inotify_add_watch(
                    socket(),
                    m_settingsPath.mid(0, m_settingsPath.lastIndexOf(QLatin1Char('/'))).toUtf8().constData(),
                    IN_CREATE);
    }

    if (m_watch < 0) {
        qWarning() << "Unable to follow devicelock configuration file changes";
    }
}

void SettingsWatcher::watchSettingsFile()
{
    m_watch = inotify_add_watch(
                socket(),
                m_settingsPath.toUtf8().constData(),
                IN_CLOSE_WRITE | IN_DELETE_SELF);
    reloadSettings();

}

template <typename T>
static void read(
        const QSettings &settings,
        SettingsWatcher *watcher,
        const char *key,
        T defaultValue,
        T (SettingsWatcher::*member),
        void (SettingsWatcher::*changed)())
{
    T value = settings.value(QString::fromUtf8(key), QVariant(defaultValue)).value<T>();

    if (watcher->*member != value) {
        watcher->*member = value;
        emit (watcher->*changed)();
    }
}

void SettingsWatcher::reloadSettings()
{
    QSettings settings(QStringLiteral("/usr/share/lipstick/devicelock/devicelock_settings.conf"), QSettings::IniFormat);

    read(settings, this, automaticLockingKey, 10, &SettingsWatcher::automaticLocking, &SettingsWatcher::automaticLockingChanged);
    read(settings, this, minimumLengthKey, 5, &SettingsWatcher::minimumLength, &SettingsWatcher::minimumLengthChanged);
    read(settings, this, maximumLengthKey, 42, &SettingsWatcher::maximumLength, &SettingsWatcher::maximumLengthChanged);
    read(settings, this, maximumAttemptsKey, -1, &SettingsWatcher::maximumAttempts, &SettingsWatcher::maximumAttemptsChanged);
    read(settings, this, peekingAllowedKey, 1, &SettingsWatcher::peekingAllowed, &SettingsWatcher::peekingAllowedChanged);
    read(settings, this, sideloadingAllowedKey, -1, &SettingsWatcher::sideloadingAllowed, &SettingsWatcher::sideloadingAllowedChanged);
    read(settings, this, showNotificationsKey, 1, &SettingsWatcher::showNotifications, &SettingsWatcher::showNotificationsChanged);
    read(settings, this, inputIsKeyboardKey, false, &SettingsWatcher::inputIsKeyboard, &SettingsWatcher::inputIsKeyboardChanged);
    read(settings, this, currentIsDigitOnlyKey, true, &SettingsWatcher::currentCodeIsDigitOnly, &SettingsWatcher::currentCodeIsDigitOnlyChanged);
}
