/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/PrecompiledHeader.h"

#include "common/Console.h"
#include "GLContext.h"
#include "glad.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef __APPLE__
#include <stdlib.h>
#else
#include <malloc.h>
#endif

#include "GLContextRetroGL.h"

static bool ShouldPreferESContext(void)
{
#ifndef _MSC_VER
	const char* value = std::getenv("PREFER_GLES_CONTEXT");
	return (value && std::strcmp(value, "1") == 0);
#else
	char buffer[2] = {};
	size_t buffer_size = sizeof(buffer);
	getenv_s(&buffer_size, buffer, "PREFER_GLES_CONTEXT");
	return (std::strcmp(buffer, "1") == 0);
#endif
}

GLContext::GLContext() { }
GLContext::~GLContext() = default;

std::unique_ptr<GLContext> GLContext::Create(gsl::span<const Version> versions_to_try)
{
	if (ShouldPreferESContext())
	{
		// move ES versions to the front
		Version* new_versions_to_try = static_cast<Version*>(alloca(sizeof(Version) * versions_to_try.size()));
		size_t count = 0;
		for (size_t i = 0; i < versions_to_try.size(); i++)
		{
			if (versions_to_try[i].profile == Profile::ES)
				new_versions_to_try[count++] = versions_to_try[i];
		}
		for (size_t i = 0; i < versions_to_try.size(); i++)
		{
			if (versions_to_try[i].profile != Profile::ES)
				new_versions_to_try[count++] = versions_to_try[i];
		}
		versions_to_try = gsl::span<const Version>(new_versions_to_try, versions_to_try.size());
	}

	std::unique_ptr<GLContext> context;
	context = ContextRetroGL::Create(versions_to_try);

	if (!context)
		return nullptr;

	Console.WriteLn("Created an %s context", context->IsGLES() ? "OpenGL ES" : "OpenGL");

	// NOTE: Not thread-safe. But this is okay, since we're not going to be creating more than one context at a time.
	static GLContext* context_being_created;
	context_being_created = context.get();

	// load up glad
	if (!context->IsGLES())
	{
		if (!gladLoadGLLoader([](const char* name) { return context_being_created->GetProcAddress(name); }))
		{
			Console.Error("Failed to load GL functions for GLAD");
			return nullptr;
		}
	}
	else
	{
		if (!gladLoadGLES2Loader([](const char* name) { return context_being_created->GetProcAddress(name); }))
		{
			Console.Error("Failed to load GLES functions for GLAD");
			return nullptr;
		}
	}

	context_being_created = nullptr;

	return context;
}
