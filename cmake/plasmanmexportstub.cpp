/*
    SPDX-FileCopyrightText: 2026 Tom Bamford <tom@bamford.io>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

// This is never compiled: its target is EXCLUDE_FROM_ALL and exists only
// because generate_export_header() derives a header from a target and a target
// needs a source file. plasma-nm generates plasmanm_editor_export.h in its own
// build, so there is no upstream file to pin, and this is how CMake is asked
// to write the consuming side of the same header.
extern "C" int plasma_nm_export_header_stub(void);

extern "C" int plasma_nm_export_header_stub(void)
{
    return 0;
}
