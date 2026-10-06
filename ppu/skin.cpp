#include <new>
#include <strings.h>
#ifndef stricmp
#define stricmp strcasecmp
#endif
#ifndef strnicmp
#define strnicmp strncasecmp
#endif
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
#include <string>
#include "skin.h"
#include "common/imagefile.h"
using namespace std;

Skin* Skin::instance = NULL;

bool Skin::loadSkin(const char* skin_path) {
	Skin* new_skin = new (std::nothrow) Skin();
	if ( !new_skin )
		return false;
	char filename[1024];
	memset(filename, 0, 1024);
	snprintf(filename, sizeof(filename), "%s%s", skin_path, "skin.xml" );
	if ( ! new_skin->load(filename) ) {
		delete new_skin;
		new_skin = NULL;
		return false;
	}
	else {
		if ( !new_skin->getBackground(skin_path) ) {
			delete new_skin;
			new_skin = NULL;
			return false;
		}
		else {
			freeSkin();
			Skin::instance = new_skin;
			return true;
		}
	}
};

void Skin::freeSkin() {
	if (Skin::instance)
		delete Skin::instance;
	Skin::instance = NULL;
};

Skin* Skin::getInstance() {
	return Skin::instance;
};

Skin::Skin() {
	background = NULL;
};

Skin::~Skin() {
	if( background ) {
		freeImage( background );
		background = NULL;
	}
};

bool Skin::load(const char* filename) {
	xmldoc = TiXmlDocument(filename);
	return xmldoc.LoadFile();
};

/********************************************************************************
 *                  get functions                                               *
 ********************************************************************************/
bool Skin::getBooleanValue(const char* name, bool default_value) {
	bool ret_value = default_value;
	
	string name_str = string(name);
	TiXmlHandle element_handle(&xmldoc);
	string::size_type pos = name_str.find_first_of('/');
	
	while(pos != string::npos) {
		string element_name_str = name_str.substr(0, pos);
		name_str = name_str.substr(pos+1, name_str.length()- 1 - pos);  
		element_handle = element_handle.FirstChild(element_name_str.c_str());
		if (!element_handle.Element()) {
			return ret_value;
			break;
		}
		pos = name_str.find_first_of('/');
	}
	
	const char* value_str = element_handle.Element()->Attribute(name_str.c_str());
	if (value_str) {
		if ( stricmp(value_str, "TRUE") == 0)
			ret_value = true;
		else
			ret_value = false;
	}
	return ret_value;
};

int Skin::getIntegerValue(const char* name, int default_value) {
	int ret_value = default_value;
	
	string name_str = string(name);
	TiXmlHandle element_handle(&xmldoc);
	string::size_type pos = name_str.find_first_of('/');
	
	while(pos != string::npos) {
		string element_name_str = name_str.substr(0, pos);
		name_str = name_str.substr(pos+1, name_str.length()- 1 - pos);  
		element_handle = element_handle.FirstChild(element_name_str.c_str());
		if (!element_handle.Element()) {
			return ret_value;
			break;
		}
		pos = name_str.find_first_of('/');
	}
	
	const char* value_str = element_handle.Element()->Attribute(name_str.c_str());
	if (value_str) {
		sscanf(value_str, "%d", &ret_value);
	}
	return ret_value;
};

float Skin::getFloatValue(const char* name, float default_value) {
	float ret_value = default_value;
	
	string name_str = string(name);
	TiXmlHandle element_handle(&xmldoc);
	string::size_type pos = name_str.find_first_of('/');
	
	while(pos != string::npos) {
		string element_name_str = name_str.substr(0, pos);
		name_str = name_str.substr(pos+1, name_str.length()- 1 - pos);  
		element_handle = element_handle.FirstChild(element_name_str.c_str());
		if (!element_handle.Element()) {
			return ret_value;
			break;
		}
		pos = name_str.find_first_of('/');
	}
	
	const char* value_str = element_handle.Element()->Attribute(name_str.c_str());
	if (value_str) {
		sscanf(value_str, "%f", &ret_value);
	}
	return ret_value;
};

const char* Skin::getStringValue(const char* name, const char* default_value) {
	const char* ret_value = default_value;
	
	string name_str = string(name);
	TiXmlHandle element_handle(&xmldoc);
	string::size_type pos = name_str.find_first_of('/');
	
	while(pos != string::npos) {
		string element_name_str = name_str.substr(0, pos);
		name_str = name_str.substr(pos+1, name_str.length()- 1 - pos);  
		element_handle = element_handle.FirstChild(element_name_str.c_str());
		if (!element_handle.Element()) {
			return ret_value;
			break;
		}
		pos = name_str.find_first_of('/');
	}
	
	const char* value_str = element_handle.Element()->Attribute(name_str.c_str());
	if (value_str) {
		ret_value = value_str;
	}
	return ret_value;
};

Color Skin::getColorValue(const char* name, Color default_value) {
	Color ret_value = default_value;
	
	string name_str = string(name);
	TiXmlHandle element_handle(&xmldoc);
	string::size_type pos = name_str.find_first_of('/');
	
	while(pos != string::npos) {
		string element_name_str = name_str.substr(0, pos);
		name_str = name_str.substr(pos+1, name_str.length()- 1 - pos);  
		element_handle = element_handle.FirstChild(element_name_str.c_str());
		if (!element_handle.Element()) {
			return ret_value;
			break;
		}
		pos = name_str.find_first_of('/');
	}
	
	const char* value_str = element_handle.Element()->Attribute(name_str.c_str());
	if (value_str) {
		sscanf(value_str, "#%06x", &ret_value);
	}
	return ret_value;
};

Color Skin::getAlphaValue(const char* name, Color default_value) {
	Color ret_value = default_value;
	
	string name_str = string(name);
	TiXmlHandle element_handle(&xmldoc);
	string::size_type pos = name_str.find_first_of('/');
	
	while(pos != string::npos) {
		string element_name_str = name_str.substr(0, pos);
		name_str = name_str.substr(pos+1, name_str.length()- 1 - pos);  
		element_handle = element_handle.FirstChild(element_name_str.c_str());
		if (!element_handle.Element()) {
			return ret_value;
			break;
		}
		pos = name_str.find_first_of('/');
	}
	
	const char* value_str = element_handle.Element()->Attribute(name_str.c_str());
	if (value_str) {
		sscanf(value_str, "#%02x", &ret_value);
	}
	return ret_value;
};

Image* Skin::getBackground(const char* skin_path) {
	if ( !background ) {
		char temp_path[1024];
		memset(temp_path, 0, 1024);
		snprintf(temp_path, sizeof(temp_path), "%s%s", skin_path, getStringValue("skin/background/image", "bg.png") );
		background = loadPNGImage( temp_path );
	}
	if ( !background ) {
		background = createImage(PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
	}
	if ( background && ( background->imageWidth != PSP_SCREEN_WIDTH || background->imageHeight != PSP_SCREEN_HEIGHT ) ) {
		freeImage(background);
		background = NULL;
		background = createImage(PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
	}
	return background;
};

