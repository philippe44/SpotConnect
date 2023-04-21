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
#include "squeeze2raop.h"

void	  	SaveConfig(char *name, void *ref, int mode);
void	   	*LoadConfig(char *name, tMRConfig *Conf, sq_dev_param_t *sq_conf);
void	  	*FindMRConfig(void *ref, char *UDN);
void 	  	*LoadMRConfig(void *ref, char *UDN, tMRConfig *Conf, sq_dev_param_t *sq_conf);
