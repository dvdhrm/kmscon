/*
 * kmscon - Module handling
 *
 * Copyright (c) 2012-2013 David Herrmann <dh.herrmann@googlemail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "kmscon_module.h"
#include "kmscon_module_interface.h"
#include "shl_dlist.h"
#include "shl_githead.h"
#include "shl_log.h"
#include "shl_misc.h"

#define LOG_SUBSYSTEM "module"

static struct shl_dlist module_list = SHL_DLIST_INIT(module_list);

int kmscon_module_open(struct kmscon_module **out, const char *file)
{
	int ret;
	struct kmscon_module *module;
	void *handle;

	if (!out || !file)
		return -EINVAL;

	log_debug("opening module %s", file);

	handle = dlopen(file, RTLD_NOW);
	if (!handle) {
		log_error("cannot open module %s (%d): %s",
			  file, errno, dlerror());
		return -EFAULT;
	}

	module = dlsym(handle, "module");
	if (!module) {
		log_error("cannot find module-info for %s", file);
		ret = -EFAULT;
		goto err_unload;
	}

	if (strcmp(module->info.githead, shl_git_head)) {
		log_error("incompatible module %s (%s != %s)",
			  file, module->info.githead, shl_git_head);
		ret = -EFAULT;
		goto err_unload;
	}

	if (module->ref != 0) {
		log_error("module %s already loaded (%ld)",
			  file, module->ref);
		ret = -EFAULT;
		goto err_unload;
	}

	log_debug("Initializing module: %s", file);

	module->ref = 1;
	module->loaded = false;
	module->handle = handle;

	module->file = strdup(file);
	if (!module->file) {
		ret = -ENOMEM;
		goto err_unload;
	}

	log_debug("  Date: %s %s", module->info.date, module->info.time);
	log_debug("  GIT: %s", module->info.githead);
	log_debug("  Hooks: %p %p %p %p",
		  module->info.init,
		  module->info.load,
		  module->info.unload,
		  module->info.exit);

	if (module->info.init) {
		ret = module->info.init();
		if (ret) {
			log_error("loading module %s failed: %d",
				  module->file, ret);
			goto err_file;
		}
	}

	*out = module;
	return 0;

err_file:
	free(module->file);
err_unload:
	dlclose(handle);
	return ret;
}

void kmscon_module_ref(struct kmscon_module *module)
{
	if (!module || !module->ref)
		return;

	++module->ref;
}

void kmscon_module_unref(struct kmscon_module *module)
{
	if (!module || !module->ref || --module->ref)
		return;

	log_debug("closing module %s", module->file);

	if (module->info.exit)
		module->info.exit();

	free(module->file);
	dlclose(module->handle);
}

int kmscon_module_load(struct kmscon_module *module)
{
	int ret;

	if (!module)
		return -EINVAL;

	if (module->loaded)
		return -EALREADY;

	log_debug("loading module %s", module->file);

	if (module->info.load)
		ret = module->info.load();
	else
		ret = 0;

	if (ret)
		return ret;

	module->loaded = true;
	return 0;
}

void kmscon_module_unload(struct kmscon_module *module)
{
	if (!module || !module->loaded)
		return;

	log_debug("unloading module %s", module->file);

	if (module->info.unload)
		module->info.unload();
	module->loaded = false;
}

void kmscon_load_modules(void)
{
	int ret;
	DIR *ent;
	struct dirent *buf, *de;
	char *file;
	struct kmscon_module *mod;

	log_debug("loading global modules from %s", BUILD_MODULE_DIR);

	if (!shl_dlist_empty(&module_list)) {
		log_error("trying to load global modules twice");
		return;
	}

	ent = opendir(BUILD_MODULE_DIR);
	if (!ent) {
		if (errno == ENOTDIR || errno == ENOENT)
			log_debug("module directory %s not available",
				  BUILD_MODULE_DIR);
		else
			log_error("cannot open module directory %s (%d): %m",
				  BUILD_MODULE_DIR, errno);
		return;
	}

	ret = shl_dirent(BUILD_MODULE_DIR, &buf);
	if (ret) {
		log_error("cannot allocate dirent object");
		closedir(ent);
		return;
	}

	while (true) {
		ret = readdir_r(ent, buf, &de);
		if (ret != 0) {
			log_error("cannot read directory %s: %d",
				  BUILD_MODULE_DIR, ret);
			break;
		} else if (!de) {
			break;
		}

		if (de->d_type == DT_DIR)
			continue;

		if (de->d_type != DT_REG &&
		    de->d_type != DT_LNK &&
		    de->d_type != DT_UNKNOWN) {
			log_warning("non-module file %s in module dir %s",
				    de->d_name, BUILD_MODULE_DIR);
			continue;
		}

		if (!shl_ends_with(de->d_name, ".so"))
			continue;

		ret = asprintf(&file, "%s/%s", BUILD_MODULE_DIR, de->d_name);
		if (ret < 0) {
			log_error("cannot allocate memory for module file name");
			continue;
		}

		ret = kmscon_module_open(&mod, file);
		free(file);

		if (ret)
			continue;

		ret = kmscon_module_load(mod);
		if (ret) {
			kmscon_module_unref(mod);
			continue;
		}

		shl_dlist_link(&module_list, &mod->list);
	}

	free(buf);
	closedir(ent);
}

void kmscon_unload_modules(void)
{
	struct kmscon_module *module;

	log_debug("unloading modules");

	while (!shl_dlist_empty(&module_list)) {
		module = shl_dlist_entry(module_list.prev, struct kmscon_module,
					 list);
		shl_dlist_unlink(&module->list);
		kmscon_module_unload(module);
		kmscon_module_unref(module);
	}
}
