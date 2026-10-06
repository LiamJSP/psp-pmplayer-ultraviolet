#include <new>
// config.cpp
#include <strings.h>
#ifndef stricmp
#define stricmp strcasecmp
#endif
/* 
 *	Copyright (C) 2006 cooleyes
 *	eyes.cooleyes@gmail.com 
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *   
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *   
 *  You should have received a copy of the GNU General Public License
 *  along with GNU Make; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA. 
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <limits.h>
#include <float.h>
#include <ctype.h>
#include <stdio.h>
#include <string>
#include "config.h"
using namespace std;

namespace {

/* Resolve the element portion of a slash-separated config path.  Legacy PPA
 * configurations predate many current options, so setters must be able to
 * create missing sections rather than silently dropping a user's change. */
static TiXmlElement *configResolveElement(TiXmlDocument *doc,
                                          std::string *path,
                                          bool create) {
    if (doc == NULL || path == NULL || path->empty())
        return NULL;

    TiXmlNode *node = doc;
    std::string remaining = *path;
    std::string::size_type pos = remaining.find_first_of('/');

    while (pos != std::string::npos) {
        const std::string elementName = remaining.substr(0, pos);
        remaining = remaining.substr(pos + 1);

        if (elementName.empty())
            return NULL;

        TiXmlElement *element = node->FirstChildElement(elementName.c_str());
        if (element == NULL && create) {
            element = new (std::nothrow) TiXmlElement(elementName.c_str());
            if (element == NULL)
                return NULL;
            if (node->LinkEndChild(element) == NULL) {
                delete element;
                return NULL;
            }
        }

        if (element == NULL)
            return NULL;

        node = element;
        pos = remaining.find_first_of('/');
    }

    *path = remaining;
    return node->ToElement();
}

/* Getters share the same path resolution as setters, without creating XML. */
static const char *configReadAttribute(TiXmlDocument *doc, const char *name) {
    std::string attribute(name ? name : "");
    TiXmlElement *element = configResolveElement(doc, &attribute, false);
    return element && !attribute.empty() ? element->Attribute(attribute.c_str()) : NULL;
}

static bool configNumberEnd(const char *start, const char *end) {
    if (start == end) return false;
    while (*end && isspace((unsigned char)*end)) ++end;
    return *end == 0;
}

} // namespace

Config* Config::instance = NULL;

bool Config::loadConfig(const char* filename) {
	Config::freeConfig();
	Config::instance = new (std::nothrow) Config();
	if ( Config::instance && Config::instance->load(filename) )
		return true;
	else {
		delete Config::instance;
		Config::instance = NULL;
		return false;
	}
};

void Config::freeConfig() {
	if (Config::instance)
		delete Config::instance;
	Config::instance = NULL;
};

Config* Config::getInstance() {
	return Config::instance;
};

Config::Config() {};
Config::~Config() {};

bool Config::load(const char* filename) {
    xmldoc = TiXmlDocument(filename);
    FILE *existing = fopen(filename, "rb");
    if (existing) {
        fclose(existing);
        return xmldoc.LoadFile();
    }
    if (errno != ENOENT) return false;
    TiXmlElement *root = new (std::nothrow) TiXmlElement("config");
    if (!root) return false;
    if (!xmldoc.LinkEndChild(root)) { delete root; return false; }
    return true;
};
	
void Config::save(const char* filename) {
	xmldoc.SaveFile(filename);
};

/********************************************************************************
 *                  get functions                                               *
 ********************************************************************************/
bool Config::getBooleanValue(const char* name, bool default_value) {
    const char *value = configReadAttribute(&xmldoc, name);
    return value ? stricmp(value, "TRUE") == 0 : default_value;
}

int Config::getIntegerValue(const char* name, int default_value) {
    const char *value = configReadAttribute(&xmldoc, name);
    if (!value) return default_value;
    char *end;
    errno = 0;
    long parsed = strtol(value, &end, 10);
    if (errno == ERANGE || !configNumberEnd(value, end) ||
        parsed < INT_MIN || parsed > INT_MAX)
        return default_value;
    return (int)parsed;
}

float Config::getFloatValue(const char* name, float default_value) {
    const char *value = configReadAttribute(&xmldoc, name);
    if (!value) return default_value;
    char *end;
    errno = 0;
    double parsed = strtod(value, &end);
    if (errno == ERANGE || !configNumberEnd(value, end) || parsed != parsed ||
        parsed < -FLT_MAX || parsed > FLT_MAX)
        return default_value;
    return (float)parsed;
}

const char* Config::getStringValue(const char* name, const char* default_value) {
    const char *value = configReadAttribute(&xmldoc, name);
    return value ? value : default_value;
}

Color Config::getColorValue(const char* name, Color default_value) {
    const char *value = configReadAttribute(&xmldoc, name);
    if (!value || value[0] != '#') return default_value;
    char *end;
    errno = 0;
    unsigned long parsed = strtoul(value + 1, &end, 16);
    if (errno == ERANGE || !configNumberEnd(value + 1, end) || parsed > 0xffffffUL)
        return default_value;
    return (Color)parsed;
}

/********************************************************************************
 *                  set functions                                               *
 ********************************************************************************/
bool Config::setBooleanValue(const char* name, bool value) {
    string attribute(name ? name : "");
    TiXmlElement *element = configResolveElement(&xmldoc, &attribute, true);
    if (element == NULL || attribute.empty())
        return false;
    element->SetAttribute(attribute.c_str(), value ? "TRUE" : "FALSE");
    return true;
}

bool Config::setIntegerValue(const char* name, int value) {
    string attribute(name ? name : "");
    TiXmlElement *element = configResolveElement(&xmldoc, &attribute, true);
    if (element == NULL || attribute.empty())
        return false;
    element->SetAttribute(attribute.c_str(), value);
    return true;
}

bool Config::setFloatValue(const char* name, float value) {
    string attribute(name ? name : "");
    TiXmlElement *element = configResolveElement(&xmldoc, &attribute, true);
    char valueString[64];
    if (element == NULL || attribute.empty())
        return false;
    snprintf(valueString, sizeof(valueString), "%.7g", (double)value);
    element->SetAttribute(attribute.c_str(), valueString);
    return true;
}

bool Config::setStringValue(const char* name, const char* value) {
    string attribute(name ? name : "");
    TiXmlElement *element = configResolveElement(&xmldoc, &attribute, true);
    if (element == NULL || attribute.empty() || value == NULL)
        return false;
    element->SetAttribute(attribute.c_str(), value);
    return true;
}

bool Config::setColorValue(const char* name, Color value) {
    string attribute(name ? name : "");
    TiXmlElement *element = configResolveElement(&xmldoc, &attribute, true);
    char valueString[64];
    if (element == NULL || attribute.empty())
        return false;
    snprintf(valueString, sizeof(valueString), "#%06x",
             (unsigned int)(value & 0x00ffffffU));
    element->SetAttribute(attribute.c_str(), valueString);
    return true;
}
