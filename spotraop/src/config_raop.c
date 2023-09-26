/*
 *  SpotRaop - Spotify to AirPlay bridge
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
#include "spotraop.h"
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
void SaveConfig(char *name, void *ref, bool full) {
	struct sMR *p;
	IXML_Document *doc = ixmlDocument_createDocument();
	IXML_Document *old_doc = ref;
	IXML_Node	 *root, *common;
	IXML_Element* old_root = ixmlDocument_getElementById(old_doc, "spotraop");

	if (!full && old_doc) {
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
		root = XMLAddNode(doc, NULL, "spotraop", NULL);
		common = (IXML_Node*) XMLAddNode(doc, root, "common", NULL);
	}

		XMLUpdateNode(doc, root, false, "slimproto_log", level2debug(slimproto_loglevel));
	XMLUpdateNode(doc, root, false, "stream_log", level2debug(stream_loglevel));
	XMLUpdateNode(doc, root, false, "output_log", level2debug(output_loglevel));
	XMLUpdateNode(doc, root, false, "decode_log", level2debug(decode_loglevel));
	XMLUpdateNode(doc, root, false, "main_log",level2debug(main_loglevel));
	XMLUpdateNode(doc, root, false, "slimmain_log", level2debug(slimmain_loglevel));
	XMLUpdateNode(doc, root, false, "raop_log",level2debug(raop_loglevel));
	XMLUpdateNode(doc, root, false, "util_log",level2debug(util_loglevel));
	XMLUpdateNode(doc, root, false, "log_limit", "%d", (int32_t) glLogLimit);

	XMLUpdateNode(doc, root, false, "interface", glInterface);
	XMLUpdateNode(doc, root, false, "ports", "%hu:%hu", glPortBase, glPortRange);
	XMLUpdateNode(doc, root, false, "credentials_path", glCredentialsPath);
	XMLUpdateNode(doc, root, false, "credentials", "%d", glCredentials);

	XMLUpdateNode(doc, common, false, "enabled", "%d", (int) glMRConfig.Enabled);
	XMLUpdateNode(doc, common, false, "volume_feedback", "%d", (int) glMRConfig.VolumeFeedback);
	XMLUpdateNode(doc, common, false, "volume_mode", "%d", (int) glMRConfig.VolumeMode);
	XMLUpdateNode(doc, common, false, "send_metadata", "%d", (int) glMRConfig.SendMetaData);
	XMLUpdateNode(doc, common, false, "send_coverart", "%d", (int) glMRConfig.SendCoverArt);
	XMLUpdateNode(doc, common, false, "remove_timeout", "%d", (int) glMRConfig.RemoveTimeout);
	XMLUpdateNode(doc, common, false, "alac_encode", "%d", (int) glMRConfig.AlacEncode);
	XMLUpdateNode(doc, common, false, "encryption", "%d", (int) glMRConfig.Encryption);
	XMLUpdateNode(doc, common, false, "read_ahead", "%d", (int) glMRConfig.ReadAhead);

	for (int i = 0; i < MAX_RENDERERS; i++) {
		IXML_Node *dev_node;

		if (!glMRDevices[i].Running) continue;
		else p = &glMRDevices[i];

		// new device, add nodes
		if (!old_doc || !FindMRConfig(old_doc, p->UDN)) {
			dev_node = XMLAddNode(doc, root, "device", NULL);
			XMLAddNode(doc, dev_node, "udn", p->UDN);
			XMLAddNode(doc, dev_node, "name", p->Config.Name);
			XMLAddNode(doc, dev_node, "friendly_name", p->FriendlyName);
			XMLAddNode(doc, dev_node, "credentials", p->Config.Credentials);
			if (*p->Config.RaopCredentials) XMLAddNode(doc, dev_node, "raop_credentials", p->Config.RaopCredentials);
			//XMLAddNode(doc, dev_node, "mac", "%02x:%02x:%02x:%02x:%02x:%02x", );
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
static void LoadConfigItem(tMRConfig *Conf, char *name, char *val) {
	if (!val)return;

	if (!strcmp(name, "mac"))  {
		unsigned mac[6];
		// seems to be a Windows scanf bug, cannot support %hx
		sscanf(val,"%2x:%2x:%2x:%2x:%2x:%2x", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]);
		for (int i = 0; i < 6; i++) Conf->MAC[i] = mac[i];
	}
	if (!strcmp(name, "enabled")) Conf->Enabled = atol(val);
	if (!strcmp(name, "remove_timeout")) Conf->RemoveTimeout = atol(val);
	if (!strcmp(name, "encryption")) Conf->Encryption = atol(val);
	if (!strcmp(name, "raop_credentials")) strcpy(Conf->RaopCredentials, val);
	if (!strcmp(name, "read_ahead")) Conf->ReadAhead = atol(val);
	if (!strcmp(name, "send_metadata")) Conf->SendMetaData = atol(val);
	if (!strcmp(name, "send_coverart")) Conf->SendCoverArt = atol(val);
	if (!strcmp(name, "volume_feedback")) Conf->VolumeFeedback = atol(val);
	if (!strcmp(name, "volume_mode")) Conf->VolumeMode = atol(val);
	if (!strcmp(name, "alac_encode")) Conf->AlacEncode = atol(val);
	if (!strcmp(name, "name")) strcpy(Conf->Name, val);
}

/*----------------------------------------------------------------------------*/
static void LoadGlobalItem(char *name, char *val) {
	if (!val) return;

	if (!strcmp(name, "interface")) strncpy(glInterface, val, sizeof(glInterface));
	if (!strcmp(name, "credentials")) glCredentials = atol(val);
	if (!strcmp(name, "credentials_path")) strncpy(glCredentialsPath, val, sizeof(glCredentialsPath) - 1);
	if (!strcmp(name, "slimproto_log")) slimproto_loglevel = debug2level(val);
	if (!strcmp(name, "stream_log")) stream_loglevel = debug2level(val);
	if (!strcmp(name, "output_log")) output_loglevel = debug2level(val);
	if (!strcmp(name, "decode_log")) decode_loglevel = debug2level(val);
	if (!strcmp(name, "main_log")) main_loglevel = debug2level(val);
	if (!strcmp(name, "slimmain_log")) slimmain_loglevel = debug2level(val);
	if (!strcmp(name, "raop_log")) raop_loglevel = debug2level(val);
	if (!strcmp(name, "util_log")) util_loglevel = debug2level(val);
	if (!strcmp(name, "log_limit")) glLogLimit = atol(val);
	if (!strcmp(name, "ports")) sscanf(val, "%hu:%hu", &glPortBase, &glPortRange);
 }


/*----------------------------------------------------------------------------*/
void *FindMRConfig(void *ref, char *UDN) {
	IXML_Node *device = NULL;
	IXML_Document *doc = (IXML_Document*) ref;
	IXML_Element* elm = ixmlDocument_getElementById(doc, "spotraop");
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
void *LoadMRConfig(void *ref, char *UDN, tMRConfig *Conf) {
	IXML_Document *doc = (IXML_Document*) ref;
	IXML_Node* node = (IXML_Node*) FindMRConfig(doc, UDN);

	if (node) {
		IXML_NodeList* node_list = ixmlNode_getChildNodes(node);
		for (unsigned i = 0; i < ixmlNodeList_length(node_list); i++) {
			IXML_Node* l1_node = ixmlNodeList_item(node_list, i);
			char* n = (char*) ixmlNode_getNodeName(l1_node);
			IXML_Node* l1_1_node = ixmlNode_getFirstChild(l1_node);
			char *v = (char*) ixmlNode_getNodeValue(l1_1_node);
			LoadConfigItem(Conf, n, v);
		}
		if (node_list) ixmlNodeList_free(node_list);
	}

	return node;
}

/*----------------------------------------------------------------------------*/
void *LoadConfig(char *name, tMRConfig *Conf) {
	IXML_Document* doc = ixmlLoadDocument(name);
	if (!doc) return NULL;

	IXML_Element* elm = ixmlDocument_getElementById(doc, "spotraop");
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
			LoadConfigItem(&glMRConfig, n, v);
		}
		if (l1_node_list) ixmlNodeList_free(l1_node_list);
	}

	return doc;
 }
