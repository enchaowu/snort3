/*
** Copyright (C) 2014 Cisco and/or its affiliates. All rights reserved.
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License Version 2 as
** published by the Free Software Foundation.  You may not use, modify or
** distribute this program under any other version of the GNU General
** Public License.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/
// cursor.cc author Russ Combs <rucombs@cisco.com>

#include "cursor.h"
#include "detection/detection_util.h"
#include "protocols/packet.h"

Cursor::Cursor(Packet* p)
{
    reset(p);
}

Cursor::Cursor(const Cursor& rhs)
{
    *this = rhs;
    delta = 0;
}

void Cursor::reset(Packet* p)
{
    /* If AltDetect is set by calling the rule options which set it,
     * we should use the Alt Detect before checking for any other buffers.
     * Alt Detect will take precedence over the Alt Decode and/or packet data.
     */
    if ( Is_DetectFlag(FLAG_ALT_DETECT) )
    {
        set("pkt_data", (uint8_t *)DetectBuffer.data, DetectBuffer.len);
    }
    else if ( Is_DetectFlag(FLAG_ALT_DECODE) )
    {
        set("pkt_data", (uint8_t *)DecodeBuffer.data, DecodeBuffer.len);
    }
    else if( IsLimitedDetect(p) )
    {
        set("pkt_data", p->data, p->alt_dsize);
    }
    else
    {
        set("pkt_data", p->data, p->dsize);
    }
}

