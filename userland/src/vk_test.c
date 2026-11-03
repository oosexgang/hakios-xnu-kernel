/*
 * Turnip on the phone, the smallest whole round: Mesa's Vulkan driver for the Adreno (tools/mk-turnip.sh) loaded as a
 * Vulkan loader would load it (vk_icdGetInstanceProcAddr), an instance, the GPU as a physical device, a device and its
 * queue, a buffer in host-visible memory filled by the GPU (vkCmdFillBuffer), the submission waited for by a fence
 * (a syncobj of tools/mesa/hakios-drm), and every word of the buffer checked. Then the same fill a hundred times, timed:
 * what one submission and its wait cost. RC_JOBS=rc.jobs.vktest runs it (an image built with TURNIP=1).
 */
#include <dlfcn.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

/* (vk_icd.h's: the loader-to-driver handshake) */
typedef VkResult (VKAPI_PTR *PFN_vkNegotiateLoaderICDInterfaceVersion)(uint32_t *pVersion);

#define DRIVER "/redwood/lib/libvulkan_freedreno.dylib"
#define WORDS (1u << 20)        /* a 4 MB buffer */

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance instance;
static VkDevice device;

#define I(name) PFN_##name name = (PFN_##name)gipa(instance, #name)
#define CHECK(what, call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { \
	printf("vk_test: %s failed: VkResult %d\n", what, r_); return 1; } } while (0)

/* a crash says where: the pc and lr, as offsets into the image each falls in */
static void
where(const char *what, uintptr_t a)
{
	Dl_info info;

	if (dladdr((void *)a, &info) && info.dli_fname != NULL)
		printf("vk_test:   %s %#lx = %s+%#lx (%s+%#lx)\n", what, (unsigned long)a, info.dli_fname,
		    (unsigned long)(a - (uintptr_t)info.dli_fbase), info.dli_sname ? info.dli_sname : "?",
		    (unsigned long)(a - (uintptr_t)info.dli_saddr));
	else
		printf("vk_test:   %s %#lx\n", what, (unsigned long)a);
}

static void
crashed(int sig, siginfo_t *si, void *context)
{
	ucontext_t *uc = context;

	printf("vk_test: signal %d at address %p\n", sig, si->si_addr);
	where("pc", (uintptr_t)uc->uc_mcontext->__ss.__pc);
	where("lr", (uintptr_t)uc->uc_mcontext->__ss.__lr);
	uintptr_t fp = (uintptr_t)uc->uc_mcontext->__ss.__fp;
	for (int i = 0; i < 16 && fp != 0 && (fp & 7) == 0; i++) {
		where("frame", ((uintptr_t *)fp)[1]);
		fp = ((uintptr_t *)fp)[0];
	}
	fflush(stdout);
	_exit(128 + sig);
}

static double
now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

int
main(void)
{
	struct sigaction sa = { .sa_sigaction = crashed, .sa_flags = SA_SIGINFO };
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGABRT, &sa, NULL);
	sigaction(SIGTRAP, &sa, NULL);
	setvbuf(stdout, NULL, _IONBF, 0);
	dup2(1, 2);                     /* Mesa's own messages (stderr) beside ours */
	setenv("TU_DEBUG", "startup", 0);
	void *driver = dlopen(DRIVER, RTLD_NOW | RTLD_LOCAL);

	if (driver == NULL) {
		printf("vk_test: %s does not load: %s\n", DRIVER, dlerror());
		return 1;
	}
	gipa = (PFN_vkGetInstanceProcAddr)dlsym(driver, "vk_icdGetInstanceProcAddr");
	PFN_vkNegotiateLoaderICDInterfaceVersion negotiate =
	    (PFN_vkNegotiateLoaderICDInterfaceVersion)dlsym(driver, "vk_icdNegotiateLoaderICDInterfaceVersion");
	if (gipa == NULL) {
		printf("vk_test: no vk_icdGetInstanceProcAddr in the driver\n");
		return 1;
	}
	if (negotiate != NULL) {
		uint32_t version = 5;
		negotiate(&version);
		printf("vk_test: loader interface version %u\n", version);
	}

	I(vkCreateInstance);
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vk_test",
	    .apiVersion = VK_API_VERSION_1_1 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	CHECK("vkCreateInstance", vkCreateInstance(&ici, NULL, &instance));
	printf("vk_test: instance made\n");

	I(vkEnumeratePhysicalDevices);
	I(vkGetPhysicalDeviceProperties);
	I(vkGetPhysicalDeviceMemoryProperties);
	I(vkGetPhysicalDeviceQueueFamilyProperties);
	uint32_t count = 1;
	VkPhysicalDevice gpu = VK_NULL_HANDLE;
	VkResult er = vkEnumeratePhysicalDevices(instance, &count, &gpu);
	if ((er != VK_SUCCESS && er != VK_INCOMPLETE) || count == 0) {
		printf("vk_test: no physical device (VkResult %d, %u devices)\n", er, count);
		return 1;
	}
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(gpu, &props);
	printf("vk_test: device \"%s\", Vulkan %u.%u.%u, driver %#x, vendor %#x device %#x\n", props.deviceName,
	    VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion),
	    props.driverVersion, props.vendorID, props.deviceID);
	uint32_t families = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(gpu, &families, NULL);
	printf("vk_test: %u queue families\n", families);

	float priority = 1.0f;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0,
	    .queueCount = 1, .pQueuePriorities = &priority };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
	    .pQueueCreateInfos = &qci };
	I(vkCreateDevice);
	CHECK("vkCreateDevice", vkCreateDevice(gpu, &dci, NULL, &device));
	printf("vk_test: device made\n");

	I(vkGetDeviceQueue);
	I(vkCreateBuffer);
	I(vkGetBufferMemoryRequirements);
	I(vkAllocateMemory);
	I(vkBindBufferMemory);
	I(vkMapMemory);
	I(vkCreateCommandPool);
	I(vkAllocateCommandBuffers);
	I(vkBeginCommandBuffer);
	I(vkCmdFillBuffer);
	I(vkEndCommandBuffer);
	I(vkCreateFence);
	I(vkQueueSubmit);
	I(vkWaitForFences);
	I(vkResetFences);
	VkQueue queue;
	vkGetDeviceQueue(device, 0, 0, &queue);

	VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = WORDS * 4,
	    .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
	VkBuffer buffer;
	CHECK("vkCreateBuffer", vkCreateBuffer(device, &bci, NULL, &buffer));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(device, buffer, &req);
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
	uint32_t type = UINT32_MAX;
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
		const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) {
			type = i;
			break;
		}
	}
	if (type == UINT32_MAX) {
		printf("vk_test: no host-visible coherent memory type\n");
		return 1;
	}
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
	    .memoryTypeIndex = type };
	VkDeviceMemory memory;
	CHECK("vkAllocateMemory", vkAllocateMemory(device, &mai, NULL, &memory));
	CHECK("vkBindBufferMemory", vkBindBufferMemory(device, buffer, memory, 0));
	uint32_t *words;
	CHECK("vkMapMemory", vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, (void **)&words));
	memset(words, 0, WORDS * 4);

	VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0 };
	VkCommandPool pool;
	CHECK("vkCreateCommandPool", vkCreateCommandPool(device, &pci, NULL, &pool));
	VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
	    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkCommandBuffer cb;
	CHECK("vkAllocateCommandBuffers", vkAllocateCommandBuffers(device, &cai, &cb));
	VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	CHECK("vkBeginCommandBuffer", vkBeginCommandBuffer(cb, &begin));
	vkCmdFillBuffer(cb, buffer, 0, VK_WHOLE_SIZE, 0x600dc0de);
	CHECK("vkEndCommandBuffer", vkEndCommandBuffer(cb));
	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence;
	CHECK("vkCreateFence", vkCreateFence(device, &fci, NULL, &fence));
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
	double t0 = now_ms();
	CHECK("vkQueueSubmit", vkQueueSubmit(queue, 1, &si, fence));
	CHECK("vkWaitForFences", vkWaitForFences(device, 1, &fence, VK_TRUE, 2000000000ull));
	double t1 = now_ms();
	uint32_t right = 0, first_wrong = UINT32_MAX;
	for (uint32_t i = 0; i < WORDS; i++) {
		if (words[i] == 0x600dc0de) right++;
		else if (first_wrong == UINT32_MAX) first_wrong = i;
	}
	printf("vk_test: the GPU filled %u of %u words with 0x600dc0de in %.2f ms%s\n", right, WORDS, t1 - t0,
	    right == WORDS ? "" : " -- WRONG");
	if (first_wrong != UINT32_MAX) printf("vk_test: word %u is %#x\n", first_wrong, words[first_wrong]);

	/* a hundred more submissions of the same, each waited for */
	double best = 1e9, sum = 0;
	for (int i = 0; i < 100; i++) {
		CHECK("vkResetFences", vkResetFences(device, 1, &fence));
		double a = now_ms();
		CHECK("vkQueueSubmit", vkQueueSubmit(queue, 1, &si, fence));
		CHECK("vkWaitForFences", vkWaitForFences(device, 1, &fence, VK_TRUE, 2000000000ull));
		double d = now_ms() - a;
		sum += d;
		if (d < best) best = d;
	}
	printf("vk_test: 100 fills of 4 MB, each submitted and waited for: %.3f ms on average, %.3f at best\n", sum / 100, best);
	printf("vk_test: %s\n", right == WORDS ? "PASS" : "FAIL");
	return right == WORDS ? 0 : 1;
}
