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

#include "config.h"
#include "Discovery.hxx"
#include "Domain.hxx"
#include "ContentDirectoryService.hxx"
#include "upnpplib.hxx"
#include "system/Clock.hxx"
#include "Log.hxx"

#include <upnp/upnptools.h>

#include <string.h>

// The service type string we are looking for.
static constexpr char ContentDirectorySType[] = "urn:schemas-upnp-org:service:ContentDirectory:1";

// We don't include a version in comparisons, as we are satisfied with
// version 1
gcc_pure
static bool
isCDService(const char *st)
{
	constexpr size_t sz = sizeof(ContentDirectorySType) - 3;
	return memcmp(ContentDirectorySType, st, sz) == 0;
}

// The type of device we're asking for in search
static constexpr char MediaServerDType[] = "urn:schemas-upnp-org:device:MediaServer:1";

gcc_pure
static bool
isMSDevice(const char *st)
{
	constexpr size_t sz = sizeof(MediaServerDType) - 3;
	return memcmp(MediaServerDType, st, sz) == 0;
}

inline void
UPnPDeviceDirectory::discoExplorer()
{
	for (;;) {
		DiscoveredTask *tsk = 0;
		if (!discoveredQueue.take(tsk)) {
			discoveredQueue.workerExit();
			return;
		}

		// Device signals its existence and well-being. Perform the
		// UPnP "description" phase by downloading and decoding the
		// description document.
		char *buf;
		// LINE_SIZE is defined by libupnp's upnp.h...
		char contentType[LINE_SIZE];
		int code = UpnpDownloadUrlItem(tsk->url.c_str(), &buf, contentType);
		if (code != UPNP_E_SUCCESS) {
			continue;
		}

		// Update or insert the device
		ContentDirectoryDescriptor d(MonotonicClockS(), tsk->expires);

		{
			Error error2;
			bool success = d.Parse(tsk->url, buf, error2);
			free(buf);
			if (!success) {
				delete tsk;
				LogError(error2);
				continue;
			}
		}

		mutex.lock();
		directories[std::move(tsk->deviceId)] = std::move(d);
		mutex.unlock();

		delete tsk;
	}
}

void *
UPnPDeviceDirectory::discoExplorer(void *ctx)
{
	UPnPDeviceDirectory &directory = *(UPnPDeviceDirectory *)ctx;
	directory.discoExplorer();
	return (void*)1;
}

inline int
UPnPDeviceDirectory::OnAlive(Upnp_Discovery *disco)
{
	if (isMSDevice(disco->DeviceType) ||
	    isCDService(disco->ServiceType)) {
		DiscoveredTask *tp = new DiscoveredTask(disco);
		if (discoveredQueue.put(tp))
			return UPNP_E_FINISH;
	}

	return UPNP_E_SUCCESS;
}

inline int
UPnPDeviceDirectory::OnByeBye(Upnp_Discovery *disco)
{

	if (isMSDevice(disco->DeviceType) ||
	    isCDService(disco->ServiceType)) {
		// Device signals it is going off.
		const ScopeLock protect(mutex);
		auto it = directories.find(disco->DeviceId);
		if (it != directories.end())
			directories.erase(it);
	}

	return UPNP_E_SUCCESS;
}

// This gets called for all libupnp asynchronous events, in a libupnp
// thread context.
// Example: ContentDirectories appearing and disappearing from the network
// We queue a task for our worker thread(s)
inline int
UPnPDeviceDirectory::cluCallBack(Upnp_EventType et, void *evp)
{
	switch (et) {
	case UPNP_DISCOVERY_SEARCH_RESULT:
	case UPNP_DISCOVERY_ADVERTISEMENT_ALIVE:
		{
			Upnp_Discovery *disco = (Upnp_Discovery *)evp;
			return OnAlive(disco);
		}

	case UPNP_DISCOVERY_ADVERTISEMENT_BYEBYE:
		{
			Upnp_Discovery *disco = (Upnp_Discovery *)evp;
			return OnByeBye(disco);
		}

	default:
		// Ignore other events for now
		break;
	}

	return UPNP_E_SUCCESS;
}

bool
UPnPDeviceDirectory::expireDevices(Error &error)
{
	const ScopeLock protect(mutex);
	const unsigned now = MonotonicClockS();
	bool didsomething = false;

	for (auto it = directories.begin();
	     it != directories.end();) {
		if (now > it->second.expires) {
			it = directories.erase(it);
			didsomething = true;
		} else {
			it++;
		}
	}

	if (didsomething)
		return search(error);

	return true;
}

UPnPDeviceDirectory::UPnPDeviceDirectory(LibUPnP *_lib)
	:lib(_lib),
	 discoveredQueue("DiscoveredQueue"),
	 m_searchTimeout(2), m_lastSearch(0)
{
}

bool
UPnPDeviceDirectory::Start(Error &error)
{
	if (!discoveredQueue.start(1, discoExplorer, this)) {
		error.Set(upnp_domain, "Discover work queue start failed");
		return false;
	}

	lib->SetHandler([this](Upnp_EventType type, void *event){
			cluCallBack(type, event);
		});

	return search(error);
}

bool
UPnPDeviceDirectory::search(Error &error)
{
	const unsigned now = MonotonicClockS();
	if (now - m_lastSearch < 10)
		return true;
	m_lastSearch = now;

	// We search both for device and service just in case.
	int code = UpnpSearchAsync(lib->getclh(), m_searchTimeout,
				   ContentDirectorySType, lib);
	if (code != UPNP_E_SUCCESS) {
		error.Format(upnp_domain, code,
			     "UpnpSearchAsync() failed: %s",
			     UpnpGetErrorMessage(code));
		return false;
	}

	code = UpnpSearchAsync(lib->getclh(), m_searchTimeout,
			       MediaServerDType, lib);
	if (code != UPNP_E_SUCCESS) {
		error.Format(upnp_domain, code,
			     "UpnpSearchAsync() failed: %s",
			     UpnpGetErrorMessage(code));
		return false;
	}

	return true;
}

bool
UPnPDeviceDirectory::getDirServices(std::vector<ContentDirectoryService> &out,
				    Error &error)
{
	// Has locking, do it before our own lock
	if (!expireDevices(error))
		return false;

	const ScopeLock protect(mutex);

	for (auto dit = directories.begin();
	     dit != directories.end(); dit++) {
		for (const auto &service : dit->second.device.services) {
			if (isCDService(service.serviceType.c_str())) {
				out.emplace_back(dit->second.device, service);
			}
		}
	}

	return true;
}

bool
UPnPDeviceDirectory::getServer(const char *friendlyName,
			       ContentDirectoryService &server,
			       Error &error)
{
	// Has locking, do it before our own lock
	if (!expireDevices(error))
		return false;

	const ScopeLock protect(mutex);

	for (const auto &i : directories) {
		const auto &device = i.second.device;

		if (device.friendlyName != friendlyName)
			continue;

		for (const auto &service : device.services) {
			if (isCDService(service.serviceType.c_str())) {
				server = ContentDirectoryService(device,
								 service);
				return true;
			}
		}
	}

	error.Set(upnp_domain, "Server not found");
	return false;
}