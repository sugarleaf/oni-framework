#include <oni/messaging/messagemanager.h>
#include <oni/messaging/messagecategory.h>
#include <oni/messaging/message.h>
#include <oni/utils/sys_wrappers.h>
#include <oni/utils/memory/allocator.h>
#include <oni/utils/logger.h>
#include <oni/utils/kdlsym.h>
#include <oni/utils/ref.h>

#include <oni/framework.h>

void __dec(struct allocation_t* allocation);

void messagemanager_init(struct messagemanager_t* manager)
{
	void* (*memset)(void *s, int c, size_t n) = kdlsym(memset);

	if (!manager)
		return;

	void(*mtx_init)(struct mtx *m, const char *name, const char *type, int opts) = kdlsym(mtx_init);
	mtx_init(&manager->lock, "mmmtx", NULL, 0);

	memset(manager->categories, 0, sizeof(manager->categories));
}

int32_t messagemanager_findFreeCategoryIndex(struct messagemanager_t* manager)
{
	void(*_mtx_lock_flags)(struct mtx *m, int opts, const char *file, int line) = kdlsym(_mtx_lock_flags);
	void(*_mtx_unlock_flags)(struct mtx *m, int opts, const char *file, int line) = kdlsym(_mtx_unlock_flags);

	if (!manager)
		return -1;

	int32_t result = -1;

	_mtx_lock_flags(&manager->lock, 0, __FILE__, __LINE__);
	for (uint32_t i = 0; i < RPCDISPATCHER_MAX_CATEGORIES; ++i)
	{
		if (!manager->categories[i])
		{
			result = i;
			break;
		}
	}
	_mtx_unlock_flags(&manager->lock, 0, __FILE__, __LINE__);

	return result;
}

uint32_t messagemanager_freeCategoryCount(struct messagemanager_t* manager)
{
	void(*_mtx_lock_flags)(struct mtx *m, int opts, const char *file, int line) = kdlsym(_mtx_lock_flags);
	void(*_mtx_unlock_flags)(struct mtx *m, int opts, const char *file, int line) = kdlsym(_mtx_unlock_flags);

	if (!manager)
		return 0;

	uint32_t clientCount = 0;
	_mtx_lock_flags(&manager->lock,0,  __FILE__, __LINE__);
	for (uint32_t i = 0; i < RPCDISPATCHER_MAX_CATEGORIES; ++i)
	{
		if (manager->categories[i])
			clientCount++;
	}
	_mtx_unlock_flags(&manager->lock, 0, __FILE__, __LINE__);

	return clientCount;
}

struct messagecategory_t* messagemanager_getCategory(struct messagemanager_t* manager, uint32_t category)
{
	void(*_mtx_lock_flags)(struct mtx *m, int opts, const char *file, int line) = kdlsym(_mtx_lock_flags);
	void(*_mtx_unlock_flags)(struct mtx *m, int opts, const char *file, int line) = kdlsym(_mtx_unlock_flags);

	if (!manager)
		return 0;

	if (category >= RPCCAT_MAX)
		return 0;

	struct messagecategory_t* rpccategory = 0;

	_mtx_lock_flags(&manager->lock, 0, __FILE__, __LINE__);
	for (uint32_t i = 0; i < RPCDISPATCHER_MAX_CATEGORIES; ++i)
	{
		struct messagecategory_t* cat = manager->categories[i];

		if (!cat)
			continue;

		if (cat->category == category)
		{
			rpccategory = manager->categories[i];
			break;
		}
	}
	_mtx_unlock_flags(&manager->lock, 0, __FILE__, __LINE__);

	return rpccategory;
}

int32_t messagemanager_registerCallback(struct messagemanager_t* manager, uint32_t callbackCategory, uint32_t callbackType, void* callback)
{
	// Verify that the dispatcher and listener are valid
	if (!manager || !callback)
		return 0;

	// Verify the listener category
	if (callbackCategory >= RPCCAT_MAX)
		return 0;

	struct messagecategory_t* category = messagemanager_getCategory(manager, callbackCategory);
	if (!category)
	{
		// Get a free listener index
		int32_t freeIndex = messagemanager_findFreeCategoryIndex(manager);
		if (freeIndex == -1)
			return 0;

		// Allocate a new category
		category = (struct messagecategory_t*)kmalloc(sizeof(struct messagecategory_t));
		if (!category)
			return 0;

		// Initialize the category
		rpccategory_init(category, callbackCategory);

		// Set the category in our list
		manager->categories[freeIndex] = category;
	}

	// Get the next free listener
	int32_t callbackIndex = rpccategory_findFreeCallbackIndex(category);
	if (callbackIndex == -1)
		return 0;

	// Install the listener to the category
	struct messagecategory_callback_t* categoryCallback = (struct messagecategory_callback_t*)kmalloc(sizeof(struct messagecategory_callback_t));
	if (!categoryCallback)
		return 0;

	// Set the type and callback
	categoryCallback->type = callbackType;
	categoryCallback->callback = callback;

	// Install our callback
	category->callbacks[callbackIndex] = categoryCallback;

	return 1;
}

int32_t messagemanager_unregisterCallback(struct messagemanager_t* manager, int32_t callbackCategory, int32_t callbackType, void* callback)
{
	// Verify that the dispatcher and listener are valid
	if (!manager || !callback)
		return false;

	// Verify the listener category
	if (callbackCategory >= RPCCAT_MAX)
		return false;

	struct messagecategory_t* category = messagemanager_getCategory(manager, callbackCategory);
	if (!category)
		return false;

	for (uint32_t l_CallbackIndex = 0; l_CallbackIndex < ARRAYSIZE(category->callbacks); ++l_CallbackIndex)
	{
		// Get the category callback structure
		struct messagecategory_callback_t* l_Callback = category->callbacks[l_CallbackIndex];

		// Check to see if this callback is used at all
		if (!l_Callback)
			continue;

		// Check the type of the message
		if (l_Callback->type != callbackType)
			continue;

		// Check the callback
		if (l_Callback->callback != callback)
			continue;

		// Remove from the list
		category->callbacks[l_CallbackIndex] = NULL;

		// Free the memory
		kfree(l_Callback, sizeof(*l_Callback));

		l_Callback = NULL;

		return true;
	}

	return false;
}

void messagemanager_sendMessageInternal(struct ref_t* msg)
{
	// Verify the message manager and message are valid
	if (!gFramework || !gFramework->messageManager || !msg)
		return;

	struct messagemanager_t* manager = gFramework->messageManager;

	struct message_t* message = ref_getIncrement(msg);
	if (!message)
	{
		WriteLog(LL_Error, "could not get reference to message");
		return; // On initial get, don't jump to cleanup
	}

	// Validate our message category
	if (message->header.category >= RPCCAT_MAX)
	{
		WriteLog(LL_Error, "[-] invalid message category: %d max: %d", message->header.category, RPCCAT_MAX);
		goto cleanup;
	}

	struct messagecategory_t* category = messagemanager_getCategory(manager, message->header.category);
	if (!category)
	{
		WriteLog(LL_Debug, "[-] could not get dispatcher category");
		goto cleanup;
	}

	// Iterate through all of the callbacks
	for (uint32_t l_CallbackIndex = 0; l_CallbackIndex < RPCCATEGORY_MAX_CALLBACKS; ++l_CallbackIndex)
	{
		// Get the category callback structure
		struct messagecategory_callback_t* l_Callback = category->callbacks[l_CallbackIndex];

		// Check to see if this callback is used at all
		if (!l_Callback)
			continue;

		// Check the type of the message
		if (l_Callback->type != message->header.error_type)
			continue;

		// Call the callback with the provided message
		WriteLog(LL_Debug, "[+] calling callback %p(%p)", l_Callback->callback, msg);
		l_Callback->callback(msg);
	}

cleanup:
	ref_release(msg);
}

void messagemanager_sendRequestLocal(struct ref_t* msg)
{
	if (!msg)
		return;

	struct message_t* message = ref_getIncrement(msg);
	if (!message)
		return;

	message->header.request = true;

	messagemanager_sendMessageInternal(msg);

	ref_release(msg);
}

void messagemanager_sendResponseLocal(struct ref_t* msg, int32_t error)
{
	if (!msg)
		return;

	struct message_t* message = ref_getIncrement(msg);
	if (!message)
		return;
	
	message->header.error_type = error;
	message->header.request = false;

	messagemanager_sendMessageInternal(msg);

	ref_release(msg);
}


void messagemanager_sendResponse(struct ref_t* msg, int32_t error)
/*
	messagemanager_sendResponse

	msg - Reference counted message_t
	error - Error to set this message to

	This function only sends back the message header with NO payload data
	If payload data is to be sent, it will need to be sent directly after this call to sendResponse
*/
{
	if (!msg)
		return;

	struct message_t* message = ref_getIncrement(msg);
	if (!message)
		return;

	// Send the response back over the socket
	if (message->socket > 0)
	{
		// Save the payload length
		uint16_t payloadLength = message->header.payloadSize;

		// Set set the payload length to 0 because we are not sending a payload
		message->header.payloadSize = 0;

		// Send response back to the PC
		kwrite(message->socket, &message->header, sizeof(message->header));

		// Set the payload length back so memory cleanup will happen normally
		message->header.payloadSize = payloadLength;
	}

	messagemanager_sendResponseLocal(msg, error);

	ref_release(msg);
}

void messagemanager_sendRequest(struct ref_t* msg)
{
	// We don't support requesting data from Mira to PC
	messagemanager_sendRequestLocal(msg);
}