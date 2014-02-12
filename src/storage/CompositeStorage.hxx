/*
 * Copyright (C) 2003-2014 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef MPD_COMPOSITE_STORAGE_HXX
#define MPD_COMPOSITE_STORAGE_HXX

#include "check.h"
#include "StorageInterface.hxx"
#include "thread/Mutex.hxx"
#include "Compiler.h"

#include <string>
#include <map>

class Error;
class Storage;

/**
 * A #Storage implementation that combines multiple other #Storage
 * instances in one virtual tree.  It is used to "mount" new #Storage
 * instances into the storage tree.
 *
 * This class is thread-safe: mounts may be added and removed at any
 * time in any thread.
 */
class CompositeStorage final : public Storage {
	/**
	 * A node in the virtual directory tree.
	 */
	struct Directory {
		/**
		 * The #Storage mounted n this virtual directory.  All
		 * "leaf" Directory instances must have a #Storage.
		 * Other Directory instances may have one, and child
		 * mounts will be "mixed" in.
		 */
		Storage *storage;

		std::map<std::string, Directory> children;

		Directory():storage(nullptr) {}
		~Directory();

		gcc_pure
		bool IsEmpty() const {
			return storage == nullptr && children.empty();
		}

		gcc_pure
		const Directory *Find(const char *uri) const;

		Directory &Make(const char *uri);

		bool Unmount();
		bool Unmount(const char *uri);

		gcc_pure
		bool MapToRelativeUTF8(std::string &buffer,
				       const char *uri) const;
	};

	struct FindResult {
		const Directory *directory;
		const char *uri;
	};

	/**
	 * Protects the virtual #Directory tree.
	 *
	 * TODO: use readers-writer lock
	 */
	mutable Mutex mutex;

	Directory root;

	mutable std::string relative_buffer;

public:
	CompositeStorage();
	virtual ~CompositeStorage();

	void Mount(const char *uri, Storage *storage);
	bool Unmount(const char *uri);

	/* virtual methods from class Storage */
	virtual bool GetInfo(const char *uri, bool follow, FileInfo &info,
			     Error &error) override;

	virtual StorageDirectoryReader *OpenDirectory(const char *uri,
						      Error &error) override;

	virtual std::string MapUTF8(const char *uri) const override;

	virtual AllocatedPath MapFS(const char *uri) const override;

	virtual const char *MapToRelativeUTF8(const char *uri) const override;

private:
	gcc_pure
	FindResult FindStorage(const char *uri) const;
	FindResult FindStorage(const char *uri, Error &error) const;

	const char *MapToRelativeUTF8(const Directory &directory,
				      const char *uri) const;
};

#endif