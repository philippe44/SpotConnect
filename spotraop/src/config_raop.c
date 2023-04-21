/*
 *  Squeeze2raop - Squeezelite to AirPlay bridge
 *
 *  (c) Philippe, philippe_44@outlook.com
 *
 *  See LICENSE
 *
 */

#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include "ixmlextra.h"
#include "squeeze2raop.h"
#include "config_raop.h"
#include "cross_log.h"

/*----------------------------------------------------------------------------*/
/* globals */
/*----------------------------------------------------------------------------*/

extern log_level	slimproto_loglevel;
extern log_level	stream_loglevel;
extern log_level	decode_loglevel;
extern log_level	output_loglevel;
extern log_level	main_loglevel;
extern log_level	slimmain_loglevel;
extern log_level	util_loglevel;
extern log_level	raop_loglevel;
extern bool 		log_cmdline;

/*----------------------------------------------------------------------------*/
/* locals */
/*----------------------------------------------------------------------------*/
extern log_level	util_loglevel;
static log_level __attribute__((unused)) * loglevel = &util_loglevel;

/*----------------------------------------------------------------------------*/
void SaveConfig(char *name, void *ref, int mode) {
	struct sMR *p;
	IXML_Document *doc = ixmlDocument_createDocument();
	IXML_Document *old_doc = ref;
	IXML_Node	 *root, *common;
	bool force = (mode == CONFIG_MIGRATE);

	IXML_Element* old_root = ixmlDocument_getElementById(old_doc, "squeeze2raop");

	if (mode != CONFIG_CREATE && old_doc) {
		ixmlDocument_importNode(doc, (IXML_Node*) old_root, true, &root);
		ixmlNode_appendChild((IXML_Node*) doc, root);

		IXML_NodeList* list = ixmlDocument_getElementsByTagName((IXML_Document*) root, "device");
		for (int i = 0; i < (int) ixmlNodeList_length(list); i++) {
			IXML_Node *device;

			device = ixmlNodeList_item(list, i);
			ixmlNode_removeChild(root, device, &device);
			ixmlNode_free(device);
		}
		if (list) ixmlNodeList_free(list);
		common = (IXML_Node*) ixmlDocument_getElementById((IXML_Document*) root, "common");
	}
	else {
		root = XMLAddNode(doc, NULL, "squeeze2raop", NULL);
		common = (IXML_Node*) XMLAddNode(doc, root, "common", NULL);
	}

	XMLUpdateNode(doc, root, force, "interface", glInterface);
	// do not update log if cmd line has set it
	if (!log_cmdline) {
		XMLUpdateNode(doc, root, force, "slimproto_log", level2debug(slimproto_loglevel));
		XMLUpdateNode(doc, root, force, "stream_log", level2debug(stream_loglevel));
		XMLUpdateNode(doc, root, force, "output_log", level2debug(output_loglevel));
		XMLUpdateNode(doc, root, force, "decode_log", level2debug(decode_loglevel));
		XMLUpdateNode(doc, root, force, "main_log",level2debug(main_loglevel));
		XMLUpdateNode(doc, root, force, "slimmain_log", level2debug(slimmain_loglevel));
		XMLUpdateNode(doc, root, force, "raop_log",level2debug(raop_loglevel));
		XMLUpdateNode(doc, root, force, "util_log",level2debug(util_loglevel));
	}
	XMLUpdateNode(doc, root, force, "log_limit", "%d", (int32_t) glLogLimit);
	XMLUpdateNode(doc, root, true, "migration", "%d", (int32_t) glMigration);
	XMLUpdateNode(doc, root, force, "ports", glPortOpen);

	XMLUpdateNode(doc, common, force, "streambuf_size", "%d", (uint32_t) glDeviceParam.streambuf_size);
	XMLUpdateNode(doc, common, force, "output_size", "%d", (uint32_t) glDeviceParam.outputbuf_size);
	XMLUpdateNode(doc, common, force, "enabled", "%d", (int) glMRConfig.Enabled);
	XMLUpdateNode(doc, common, force, "codecs", glDeviceParam.codecs);
	XMLUpdateNode(doc, common, force, "sample_rate", "%d", (int) glDeviceParam.sample_rate);
	XMLUpdateNode(doc, common, force, "resolution", glDeviceParam.resolution);
#if defined(RESAMPLE)
	XMLUpdateNode(doc, common, force, "resample", "%d", (int) glDeviceParam.resample);
	XMLUpdateNode(doc, common, force, "resample_options", glDeviceParam.resample_options);
#endif
	XMLUpdateNode(doc, common, force, "player_volume", "%d", (int) glMRConfig.Volume);
	XMLUpdateNode(doc, common, force, "volume_mapping", glMRConfig.VolumeMapping);
	XMLUpdateNode(doc, common, force, "volume_feedback", "%d", (int) glMRConfig.VolumeFeedback);
	XMLUpdateNode(doc, common, force, "volume_mode", "%d", (int) glMRConfig.VolumeMode);
	XMLUpdateNode(doc, common, force, "mute_on_pause", "%d", (int) glMRConfig.MuteOnPause);
	XMLUpdateNode(doc, common, force, "send_metadata", "%d", (int) glMRConfig.SendMetaData);
	XMLUpdateNode(doc, common, force, "send_coverart", "%d", (int) glMRConfig.SendCoverArt);
	XMLUpdateNode(doc, common, force, "auto_play", "%d", (int) glMRConfig.AutoPlay);
	XMLUpdateNode(doc, common, force, "idle_timeout", "%d", (int) glMRConfig.IdleTimeout);
	XMLUpdateNode(doc, common, force, "remove_timeout", "%d", (int) glMRConfig.RemoveTimeout);
	XMLUpdateNode(doc, common, force, "alac_encode", "%d", (int) glMRConfig.AlacEncode);
	XMLUpdateNode(doc, common, force, "encryption", "%d", (int) glMRConfig.Encryption);
	XMLUpdateNode(doc, common, force, "read_ahead", "%d", (int) glMRConfig.ReadAhead);
	XMLUpdateNode(doc, common, force, "server", glDeviceParam.server);

	// correct some buggy parameters
	if (glDeviceParam.sample_rate < 44100) XMLUpdateNode(doc, common, true, "sample_rate", "%d", 96000);

	for (int i = 0; i < MAX_RENDERERS; i++) {
		IXML_Node *dev_node;

		if (!glMRDevices[i].Running) continue;
		else p = &glMRDevices[i];

		// existing device, keep param and update "name" if LMS has requested it
		if (old_doc && ((dev_node = (IXML_Node*) FindMRConfig(old_doc, p->UDN)) != NULL)) {
			ixmlDocument_importNode(doc, dev_node, true, &dev_node);
			ixmlNode_appendChild((IXML_Node*) root, dev_node);

			XMLUpdateNode(doc, dev_node, true, "friendly_name", p->FriendlyName);
			XMLUpdateNode(doc, dev_node, true, "name", p->sq_config.name);
			if (*p->Config.Credentials) XMLUpdateNode(doc, dev_node, true, "credentials", p->Config.Credentials);
			if (*p->sq_config.dynamic.server) XMLUpdateNode(doc, dev_node, true, "server", p->sq_config.dynamic.server);
		}
		// new device, add nodes
		else {
			dev_node = XMLAddNode(doc, root, "device", NULL);
			XMLAddNode(doc, dev_node, "udn", p->UDN);
			XMLAddNode(doc, dev_node, "name", p->FriendlyName);
			XMLAddNode(doc, dev_node, "friendly_name", p->FriendlyName);
			if (*p->Config.Credentials) XMLAddNode(doc, dev_node, "credentials", p->Config.Credentials);
			if (*p->sq_config.dynamic.server) XMLAddNode(doc, dev_node, "server", p->sq_config.dynamic.server);
			XMLAddNode(doc, dev_node, "mac", "%02x:%02x:%02x:%02x:%02x:%02x", p->sq_config.mac[0],
						p->sq_config.mac[1], p->sq_config.mac[2], p->sq_config.mac[3], p->sq_config.mac[4], p->sq_config.mac[5]);
			XMLAddNode(doc, dev_node, "enabled", "%d", (int) p->Config.Enabled);
		}
	}

	// add devices in old XML file that has not been discovered
	IXML_NodeList* list = ixmlDocument_getElementsByTagName((IXML_Document*) old_root, "device");
	for (int i = 0; i < (int) ixmlNodeList_length(list); i++) {
		char *udn;
		IXML_Node *device, *node;

		device = ixmlNodeList_item(list, i);
		node = (IXML_Node*) ixmlDocument_getElementById((IXML_Document*) device, "udn");
		node = ixmlNode_getFirstChild(node);
		udn = (char*) ixmlNode_getNodeValue(node);
		if (!FindMRConfig(doc, udn)) {
			ixmlDocument_importNode(doc, (IXML_Node*) device, true, &device);
			ixmlNode_appendChild((IXML_Node*) root, device);
		}
	}
	if (list) ixmlNodeList_free(list);

	FILE* file = fopen(name, "wb");
	char* s = ixmlDocumenttoString(doc);
	fwrite(s, 1, strlen(s), file);
	fclose(file);
	free(s);

	ixmlDocument_free(doc);
}


/*----------------------------------------------------------------------------*/
static void LoadConfigItem(tMRConfig *Conf, sq_dev_param_t *sq_conf, char *name, char *val) {
	if (!val)return;

	if (!strcmp(name, "streambuf_size")) sq_conf->streambuf_size = atol(val);
	if (!strcmp(name, "output_size")) sq_conf->outputbuf_size = atol(val);
	if (!strcmp(name, "codecs")) strcpy(sq_conf->codecs, val);
	if (!strcmp(name, "sample_rate")) sq_conf->sample_rate = atol(val);
	if (!strcmp(name, "name")) strcpy(sq_conf->name, val);
	if (!strcmp(name, "server")) strcpy(sq_conf->server, val);
	if (!strcmp(name, "resolution")) strcpy(sq_conf->resolution, val);
	if (!strcmp(name, "mac"))  {
		unsigned mac[6];
		// seems to be a Windows scanf buf, cannot support %hhx
		sscanf(val,"%2x:%2x:%2x:%2x:%2x:%2x", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]);
		for (int i = 0; i < 6; i++) sq_conf->mac[i] = mac[i];
	}
#if defined(RESAMPLE)
	if (!strcmp(name, "resample")) sq_conf->resample = atol(val);
	if (!strcmp(name, "resample_options")) strcpy(sq_conf->resample_options, val);
#endif
	if (!strcmp(name, "enabled")) Conf->Enabled = atol(val);
	if (!strcmp(name, "auto_play")) Conf->AutoPlay = atol(val);
	if (!strcmp(name, "idle_timeout")) Conf->IdleTimeout = atol(val);
	if (!strcmp(name, "remove_timeout")) Conf->RemoveTimeout = atol(val);
	if (!strcmp(name, "encryption")) Conf->Encryption = atol(val);
	if (!strcmp(name, "credentials")) strcpy(Conf->Credentials, val);
	if (!strcmp(name, "read_ahead")) Conf->ReadAhead = atol(val);
	if (!strcmp(name, "send_metadata")) Conf->SendMetaData = atol(val);
	if (!strcmp(name, "send_coverart")) Conf->SendCoverArt = atol(val);
//	if (!strcmp(name, "friendly_name")) strcpy(Conf->Name, val);
	if (!strcmp(name, "player_volume")) Conf->Volume = atol(val);
	if (!strcmp(name, "volume_mapping")) strcpy(Conf->VolumeMapping, val);
	if (!strcmp(name, "volume_feedback")) Conf->VolumeFeedback = atol(val);
	if (!strcmp(name, "volume_mode")) Conf->VolumeMode = atol(val);
	if (!strcmp(name, "mute_on_pause")) Conf->MuteOnPause = atol(val);
	if (!strcmp(name, "alac_encode")) Conf->AlacEncode = atol(val);
}

/*----------------------------------------------------------------------------*/
static void LoadGlobalItem(char *name, char *val) {
	if (!val) return;

	if (!strcmp(name, "interface")) strcpy(glInterface, val);
	if (!strcmp(name, "slimproto_log")) slimproto_loglevel = debug2level(val);
	if (!strcmp(name, "stream_log")) stream_loglevel = debug2level(val);
	if (!strcmp(name, "output_log")) output_loglevel = debug2level(val);
	if (!strcmp(name, "decode_log")) decode_loglevel = debug2level(val);
	if (!strcmp(name, "main_log")) main_loglevel = debug2level(val);
	if (!strcmp(name, "slimmain_log")) slimmain_loglevel = debug2level(val);
	if (!strcmp(name, "raop_log")) raop_loglevel = debug2level(val);
	if (!strcmp(name, "util_log")) util_loglevel = debug2level(val);
	if (!strcmp(name, "log_limit")) glLogLimit = atol(val);
	if (!strcmp(name, "exclude_model")) strcpy(glExcluded, val);
	if (!strcmp(name, "migration")) glMigration = atol(val);
	if (!strcmp(name, "ports")) strcpy(glPortOpen, val);
 }


/*----------------------------------------------------------------------------*/
void *FindMRConfig(void *ref, char *UDN) {
	IXML_Node *device = NULL;
	IXML_Document *doc = (IXML_Document*) ref;
	IXML_Element* elm = ixmlDocument_getElementById(doc, "squeeze2raop");
	IXML_NodeList* l1_node_list = ixmlDocument_getElementsByTagName((IXML_Document*) elm, "udn");

	for (unsigned i = 0; i < ixmlNodeList_length(l1_node_list); i++) {
		IXML_Node* l1_node = ixmlNodeList_item(l1_node_list, i);
		IXML_Node* l1_1_node = ixmlNode_getFirstChild(l1_node);
		char* v = (char*) ixmlNode_getNodeValue(l1_1_node);
		if (v && !strcmp(v, UDN)) {
			device = ixmlNode_getParentNode(l1_node);
			break;
		}
	}
	if (l1_node_list) ixmlNodeList_free(l1_node_list);
	return device;
}

/*----------------------------------------------------------------------------*/
void *LoadMRConfig(void *ref, char *UDN, tMRConfig *Conf, sq_dev_param_t *sq_conf) {
	IXML_Document *doc = (IXML_Document*) ref;
	IXML_Node* node = (IXML_Node*) FindMRConfig(doc, UDN);

	if (node) {
		IXML_NodeList* node_list = ixmlNode_getChildNodes(node);
		for (unsigned i = 0; i < ixmlNodeList_length(node_list); i++) {
			IXML_Node* l1_node = ixmlNodeList_item(node_list, i);
			char* n = (char*) ixmlNode_getNodeName(l1_node);
			IXML_Node* l1_1_node = ixmlNode_getFirstChild(l1_node);
			char *v = (char*) ixmlNode_getNodeValue(l1_1_node);
			LoadConfigItem(Conf, sq_conf, n, v);
		}
		if (node_list) ixmlNodeList_free(node_list);
	}

	return node;
}

/*----------------------------------------------------------------------------*/
void *LoadConfig(char *name, tMRConfig *Conf, sq_dev_param_t *sq_conf) {
	IXML_Document* doc = ixmlLoadDocument(name);
	if (!doc) return NULL;

	IXML_Element* elm = ixmlDocument_getElementById(doc, "squeeze2raop");
	if (elm) {
		IXML_NodeList* l1_node_list = ixmlNode_getChildNodes((IXML_Node*) elm);
		for (unsigned i = 0; i < ixmlNodeList_length(l1_node_list); i++) {
			IXML_Node* l1_node = ixmlNodeList_item(l1_node_list, i);
			char* n = (char*) ixmlNode_getNodeName(l1_node);
			IXML_Node* l1_1_node = ixmlNode_getFirstChild(l1_node);
			char* v = (char*) ixmlNode_getNodeValue(l1_1_node);
			LoadGlobalItem(n, v);
		}
		if (l1_node_list) ixmlNodeList_free(l1_node_list);
	}

	elm = ixmlDocument_getElementById((IXML_Document	*)elm, "common");
	if (elm) {
		IXML_NodeList* l1_node_list = ixmlNode_getChildNodes((IXML_Node*) elm);
		for (unsigned i = 0; i < ixmlNodeList_length(l1_node_list); i++) {
			IXML_Node* l1_node = ixmlNodeList_item(l1_node_list, i);
			char* n = (char*) ixmlNode_getNodeName(l1_node);
			IXML_Node* l1_1_node = ixmlNode_getFirstChild(l1_node);
			char* v = (char*) ixmlNode_getNodeValue(l1_1_node);
			LoadConfigItem(&glMRConfig, &glDeviceParam, n, v);
		}
		if (l1_node_list) ixmlNodeList_free(l1_node_list);
	}

	return doc;
 }



