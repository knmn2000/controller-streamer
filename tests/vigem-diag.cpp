// controller-streamer - stream a game controller over the LAN to a Windows PC
// Copyright (C) 2026 knmn2000
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
// vigem-diag.cpp - print the EXACT VIGEM_ERROR from connect/target_add.
// The receiver only prints "failed"; T4 needs the code to distinguish
// bus-not-found from version-mismatch from timeout.
#include <windows.h>
#include <ViGEm/Client.h>
#include <cstdio>

static const char* err_name(VIGEM_ERROR e) {
    switch (e) {
    case VIGEM_ERROR_NONE:                       return "NONE";
    case VIGEM_ERROR_BUS_NOT_FOUND:              return "BUS_NOT_FOUND";
    case VIGEM_ERROR_NO_FREE_SLOT:               return "NO_FREE_SLOT";
    case VIGEM_ERROR_INVALID_TARGET:             return "INVALID_TARGET";
    case VIGEM_ERROR_REMOVAL_FAILED:             return "REMOVAL_FAILED";
    case VIGEM_ERROR_ALREADY_CONNECTED:          return "ALREADY_CONNECTED";
    case VIGEM_ERROR_TARGET_UNINITIALIZED:       return "TARGET_UNINITIALIZED";
    case VIGEM_ERROR_TARGET_NOT_PLUGGED_IN:      return "TARGET_NOT_PLUGGED_IN";
    case VIGEM_ERROR_BUS_VERSION_MISMATCH:       return "BUS_VERSION_MISMATCH";
    case VIGEM_ERROR_BUS_ACCESS_FAILED:          return "BUS_ACCESS_FAILED";
    case VIGEM_ERROR_CALLBACK_ALREADY_REGISTERED:return "CALLBACK_ALREADY_REGISTERED";
    case VIGEM_ERROR_CALLBACK_NOT_FOUND:         return "CALLBACK_NOT_FOUND";
    case VIGEM_ERROR_BUS_ALREADY_CONNECTED:      return "BUS_ALREADY_CONNECTED";
    case VIGEM_ERROR_BUS_INVALID_HANDLE:         return "BUS_INVALID_HANDLE";
    case VIGEM_ERROR_XUSB_USERINDEX_OUT_OF_RANGE:return "XUSB_USERINDEX_OUT_OF_RANGE";
    default:                                     return "UNKNOWN";
    }
}

int main() {
    PVIGEM_CLIENT c = vigem_alloc();
    std::printf("vigem_alloc: %s\n", c ? "ok" : "NULL");
    if (!c) return 1;

    VIGEM_ERROR e = vigem_connect(c);
    std::printf("vigem_connect: 0x%08X %s\n", (unsigned)e, err_name(e));
    if (!VIGEM_SUCCESS(e)) return 1;

    for (int i = 0; i < 2; ++i) {
        PVIGEM_TARGET t = vigem_target_x360_alloc();
        std::printf("target %d alloc: %s\n", i, t ? "ok" : "NULL");
        e = vigem_target_add(c, t);
        std::printf("target %d add: 0x%08X %s   (GetLastError=%lu)\n",
                    i, (unsigned)e, err_name(e), GetLastError());
        if (VIGEM_SUCCESS(e)) {
            ULONG idx = 0;
            VIGEM_ERROR ie = vigem_target_x360_get_user_index(c, t, &idx);
            std::printf("target %d user index: %lu (0x%08X)\n", i, idx, (unsigned)ie);
        }
    }
    std::printf("sleeping 5 s - check joy.cpl now\n");
    Sleep(5000);
    vigem_disconnect(c);
    vigem_free(c);
    std::printf("done\n");
    return 0;
}
