/*
 * uterm - Linux User-Space Terminal
 *
 * Copyright (c) 2012 David Herrmann <dh.herrmann@googlemail.com>
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

/*
 * PCI Helpers
 * This uses the pciaccess library to retrieve information from the PCI bus.
 */

#include <errno.h>
#include <inttypes.h>
#include <pciaccess.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "log.h"
#include "uterm.h"
#include "uterm_pci.h"

#define LOG_SUBSYSTEM "pci"

/* pci classes */
#define UTERM_PCI_CLASS_PREHISTORIC		0x00
#define UTERM_PCI_CLASS_DISPLAY		0x03
#define UTERM_PCI_CLASS_MULTIMEDIA		0x04
#define UTERM_PCI_CLASS_PROCESSOR		0x0b

/* pci sub-classes */
#define UTERM_PCI_SUBCLASS_DISPLAY_VGA		0x00
#define UTERM_PCI_SUBCLASS_MULTIMEDIA_VIDEO	0x00
#define UTERM_PCI_SUBCLASS_PROCESSOR_COPROC	0x40

/* pci registers */
#define UTERM_PCI_CMD_MEM_ENABLE		0x02

static bool uterm_pci_is_gpu(struct pci_device *dev)
{
	uint32_t class = dev->device_class;
	uint32_t match;

	match = UTERM_PCI_CLASS_PREHISTORIC << 16;
	if ((class & 0x00ff0000) == match)
		return true;

	match = UTERM_PCI_CLASS_DISPLAY << 16;
	if ((class & 0x00ff0000) == match)
		return true;

	match = UTERM_PCI_CLASS_MULTIMEDIA << 16;
	match |= UTERM_PCI_SUBCLASS_MULTIMEDIA_VIDEO << 8;
	if ((class & 0x00ffff00) == match)
		return true;

	match = UTERM_PCI_CLASS_PROCESSOR << 16;
	match |= UTERM_PCI_SUBCLASS_PROCESSOR_COPROC << 8;
	if ((class & 0x00ffff00) == match)
		return true;

	return false;
}

static bool uterm_pci_is_vga(struct pci_device *dev)
{
	uint32_t class = dev->device_class;
	uint32_t match;

	match = UTERM_PCI_CLASS_DISPLAY << 16;
	match |= UTERM_PCI_SUBCLASS_DISPLAY_VGA << 8;
	return (class & 0x00ffff00) == match;
}

static const struct pci_slot_match uterm_pci_match = {
	.domain = PCI_MATCH_ANY,
	.bus = PCI_MATCH_ANY,
	.dev = PCI_MATCH_ANY,
	.func = PCI_MATCH_ANY,
	.match_data = 0,
};

#define UTERM_PCI_FORMAT "pci:%04x:%02x:%02x.%d"

int uterm_pci_get_primary_id(char **out)
{
	int ret;
	struct pci_device_iterator *iter;
	struct pci_device *dev;
	char *buf;
	uint16_t cmd;
	unsigned int num;

	if (!out)
		return -EINVAL;

	ret = pci_system_init();
	if (ret) {
		log_error("cannot initialize pciaccess library (%d/%d): %m",
			  ret, errno);
		return -EFAULT;
	}

	iter = pci_slot_match_iterator_create(&uterm_pci_match);
	if (!iter) {
		log_error("cannot create pci-slot iterator (%d): %m",
			  errno);
		ret = -EFAULT;
		goto out_cleanup;
	}

	buf = NULL;
	num = 0;
	while ((dev = pci_device_next(iter))) {
		if (!uterm_pci_is_gpu(dev))
			continue;

		++num;
		if (!pci_device_is_boot_vga(dev))
			continue;

		log_debug("primary PCI GPU: " UTERM_PCI_FORMAT,
			  dev->domain, dev->bus, dev->dev, dev->func);

		if (buf) {
			log_warning("multiple primary PCI GPUs found");
			continue;
		}

		ret = asprintf(&buf, UTERM_PCI_FORMAT, dev->domain, dev->bus,
			       dev->dev, dev->func);
		if (ret < 0) {
			log_error("cannot allocate memory for PCI name");
			goto out_iter;
		}
	}

	free(iter);
	if (buf) {
		ret = 0;
		*out = buf;
		goto out_cleanup;
	}

	/* If no GPU is marked as boot_vga, we try finding a VGA card */
	iter = pci_slot_match_iterator_create(&uterm_pci_match);
	if (!iter) {
		log_error("cannot create pci-slot iterator (%d): %m",
			  errno);
		ret = -EFAULT;
		goto out_cleanup;
	}

	while ((dev = pci_device_next(iter))) {
		if (!uterm_pci_is_gpu(dev))
			continue;

		ret = pci_device_cfg_read_u16(dev, &cmd, 4);
		if (ret)
			continue;
		if (!(cmd & UTERM_PCI_CMD_MEM_ENABLE))
			continue;
		if (num != 1 && !uterm_pci_is_vga(dev))
			continue;

		log_debug("primary PCI VGA GPU: " UTERM_PCI_FORMAT,
			  dev->domain, dev->bus, dev->dev, dev->func);

		if (buf) {
			log_warning("multiple primary PCI VGA GPUs found");
			continue;
		}

		ret = asprintf(&buf, UTERM_PCI_FORMAT, dev->domain, dev->bus,
			       dev->dev, dev->func);
		if (ret < 0) {
			log_error("cannot allocate memory for PCI name");
			goto out_iter;
		}
	}

	if (buf) {
		ret = 0;
		*out = buf;
	} else {
		log_warning("no primary PCI GPU found");
		ret = -ENOENT;
	}

out_iter:
	free(iter);
out_cleanup:
	pci_system_cleanup();
	return ret;
}
