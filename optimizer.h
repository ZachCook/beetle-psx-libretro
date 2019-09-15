/*
 * Copyright (C) 2014 Paul Cercueil <paul@crapouillou.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#ifndef __OPTIMIZER_H__
#define __OPTIMIZER_H__

struct opcode;

_Bool opcode_reads_register(const struct opcode *op, u8 reg);
_Bool opcode_writes_register(const struct opcode *op, u8 reg);

int lightrec_optimize(struct opcode *list);

#endif /* __OPTIMIZER_H__ */
