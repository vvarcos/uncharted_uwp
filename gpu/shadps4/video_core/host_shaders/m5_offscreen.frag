// SPDX-FileCopyrightText: Copyright 2026
// SPDX-License-Identifier: GPL-2.0-or-later
// Diagnostic only: never used to render guest frames.
#version 450
layout(location = 0) out vec4 color;
void main() {
    color = vec4(0.0, 1.0, 0.0, 1.0);
}
