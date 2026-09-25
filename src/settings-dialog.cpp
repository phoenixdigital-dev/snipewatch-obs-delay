/*
SnipeWatch Delay for OBS
Copyright (C) 2026 Phoenix Digital

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

/* Small Qt dialogs opened from the Tools menu (UI thread). */

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QInputDialog>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QString>

#include "remote.h"

static QWidget *main_window()
{
	return static_cast<QWidget *>(obs_frontend_get_main_window());
}

extern "C" void swd_open_status_dialog(void)
{
	char buf[2048];
	swd_remote_status(buf, sizeof(buf));
	QMessageBox::information(main_window(), QString::fromUtf8(obs_module_text("StatusTitle")), QString::fromUtf8(buf));
}

extern "C" void swd_open_token_dialog(void)
{
	bool ok = false;
	QString label = QString::fromUtf8(obs_module_text("TokenLabel"));
	if (swd_remote_has_token())
		label += "\n\n" + QString::fromUtf8(obs_module_text("TokenReplace"));

	QString token = QInputDialog::getText(main_window(), QString::fromUtf8(obs_module_text("TokenTitle")), label,
					      QLineEdit::Password, QString(), &ok);
	if (!ok)
		return;
	token = token.trimmed();
	if (!token.isEmpty() && !token.startsWith("swd_")) {
		QMessageBox::warning(main_window(), QString::fromUtf8(obs_module_text("TokenTitle")),
				     QString::fromUtf8(obs_module_text("TokenInvalid")));
		return;
	}
	swd_remote_set_token(token.toUtf8().constData());
	QMessageBox::information(main_window(), QString::fromUtf8(obs_module_text("TokenTitle")),
				 QString::fromUtf8(obs_module_text(token.isEmpty() ? "TokenCleared" : "TokenSaved")));
}
