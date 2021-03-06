/*
 * $QNXLicenseC: 
 * Copyright 2007, 2008, QNX Software Systems.  
 *  
 * Licensed under the Apache License, Version 2.0 (the "License"). You  
 * may not reproduce, modify or distribute this software except in  
 * compliance with the License. You may obtain a copy of the License  
 * at: http://www.apache.org/licenses/LICENSE-2.0  
 *  
 * Unless required by applicable law or agreed to in writing, software  
 * distributed under the License is distributed on an "AS IS" basis,  
 * WITHOUT WARRANTIES OF ANY KIND, either express or implied. 
 * 
 * This file may contain contributions from others, either as  
 * contributors under the License or as licensors under other terms.   
 * Please review this entire file for other proprietary rights or license  
 * notices, as well as the QNX Development Suite License Guide at  
 * http://licensing.qnx.com/license-guide/ for other information. 
 * $ 
 */



#include "ipl.h"

static ser_dev *ser;

void init_serdev(ser_dev *dev)
{
	ser = dev;
}

unsigned char 
ser_poll(void)
{
	return (ser->poll());
}

unsigned char 
ser_getchar(void)
{
	return (ser->get_byte());
}

void
ser_putchar(char c)
{
	if (c == '\n')
		ser->put_byte('\r');
	ser->put_byte(c);
}

void
ser_putstr(const char *str)
{
	while (*str)
		ser_putchar(*str++);
}

void
ser_puthex(unsigned x)
{
	int					i;
	char				buf[9];
	static const char	hex[] = "0123456789ABCDEF";

	for (i = 0; i < 8; i++) {
		buf[7-i] = hex[x & 15];
		x >>= 4;
	}
	buf[8] = '\0';
	ser_putstr(buf);
}

