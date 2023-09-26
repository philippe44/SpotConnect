/*
 *  Squeeze2raop - Squeezelite to AirPlay bridge
 *
 *  (c) Philippe, philippe_44@outlook.com
 *
 *  See LICENSE
 *
 */

#pragma once

#include "ixml.h"
#include "spotraop.h"

void	  	SaveConfig(char *name, void *ref, bool full);
void	   	*LoadConfig(char *name, tMRConfig *Conf);
void	  	*FindMRConfig(void *ref, char *UDN);
void 	  	*LoadMRConfig(void *ref, char *UDN, tMRConfig *Conf);
