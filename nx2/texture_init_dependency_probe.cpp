// Covers the initialization dependency fixed by Segmentree in PR #228.
// Build: clang++ -std=c++20 -O2 -I"%VULKAN_SDK%/Include" nx2/texture_init_dependency_probe.cpp -o texture_init_dependency_probe.exe
// Set VK_LAYER_PATH to the Khronos validation layer directory; run the executable with old, then fixed.
// API dependency probe only: no game data and no claim about coloured-noise causality.
// Run with Khronos validation installed. old: expect SYNC-HAZARD-WRITE-AFTER-WRITE;
// fixed: expect no validation errors. Uses the exact PR228 dependency scopes.
#define WIN32_LEAN_AND_MEAN
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#define CHECK(v) do {if(!(v)){std::fprintf(stderr,"FAIL:%d %s\n",__LINE__,#v);std::exit(2);}}while(0)
#define APIS(X) X(vkCreateInstance) X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) X(vkCreateDevice) X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindImageMemory) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier2) X(vkCmdClearColorImage) X(vkCmdCopyBufferToImage) X(vkQueueSubmit) X(vkQueueWaitIdle)
#define DECL(n) PFN_##n n;
APIS(DECL)
int errors=0,hazards=0;
VKAPI_ATTR VkBool32 VKAPI_CALL debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity,VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* data,void*) {
    if(severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT){errors++;std::fprintf(stderr,"%s: %s\n",data->pMessageIdName ? data->pMessageIdName : "(no message ID)",data->pMessage);}
    if((data->pMessageIdName && std::strstr(data->pMessageIdName,"SYNC-HAZARD-WRITE-AFTER-WRITE")) || std::strstr(data->pMessage,"WRITE_AFTER_WRITE"))hazards++;
    return VK_FALSE;
}
int main(int argc,char**argv) {
    const bool fixed=argc==2 && !std::strcmp(argv[1],"fixed");CHECK(fixed || (argc==2 && !std::strcmp(argv[1],"old")));
    auto loader=LoadLibraryA("vulkan-1.dll");CHECK(loader);
#define LOAD(n) n=(PFN_##n)GetProcAddress(loader,#n);CHECK(n);
    APIS(LOAD)
    auto gip=(PFN_vkGetInstanceProcAddr)GetProcAddress(loader,"vkGetInstanceProcAddr");CHECK(gip);
    VkValidationFeatureEnableEXT feature=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT vf{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};vf.enabledValidationFeatureCount=1;vf.pEnabledValidationFeatures=&feature;
    VkDebugUtilsMessengerCreateInfoEXT db{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    db.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;db.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;db.pfnUserCallback=debug;db.pNext=&vf;
    const char* layers[]={"VK_LAYER_KHRONOS_validation"};const char* extensions[]={VK_EXT_DEBUG_UTILS_EXTENSION_NAME,VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ici.pNext=&db;ici.pApplicationInfo=&app;ici.enabledLayerCount=1;ici.ppEnabledLayerNames=layers;ici.enabledExtensionCount=2;ici.ppEnabledExtensionNames=extensions;
    VkInstance instance{};CHECK(vkCreateInstance(&ici,nullptr,&instance)==VK_SUCCESS);
    auto createDebug=(PFN_vkCreateDebugUtilsMessengerEXT)gip(instance,"vkCreateDebugUtilsMessengerEXT");auto destroyDebug=(PFN_vkDestroyDebugUtilsMessengerEXT)gip(instance,"vkDestroyDebugUtilsMessengerEXT");CHECK(createDebug && destroyDebug);
    db.pNext=nullptr;VkDebugUtilsMessengerEXT messenger{};CHECK(createDebug(instance,&db,nullptr,&messenger)==VK_SUCCESS);
    uint32_t n=0;CHECK(vkEnumeratePhysicalDevices(instance,&n,nullptr)==VK_SUCCESS && n);std::vector<VkPhysicalDevice> devices(n);CHECK(vkEnumeratePhysicalDevices(instance,&n,devices.data())==VK_SUCCESS);
    auto physical=devices[0];for(auto d:devices){VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(d,&props);if(props.vendorID==0x1002)physical=d;}
    VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(physical,&props);std::printf("device: %s; mode: %s\n",props.deviceName,argv[1]);
    vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,nullptr);std::vector<VkQueueFamilyProperties> families(n);vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,families.data());uint32_t family=0;while(family<n && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))family++;CHECK(family<n);
    float priority=1;VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};f13.synchronization2=VK_TRUE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dci.pNext=&f13;dci.queueCreateInfoCount=1;dci.pQueueCreateInfos=&qi;VkDevice device{};CHECK(vkCreateDevice(physical,&dci,nullptr,&device)==VK_SUCCESS);VkQueue queue{};vkGetDeviceQueue(device,family,0,&queue);
    VkPhysicalDeviceMemoryProperties memory{};vkGetPhysicalDeviceMemoryProperties(physical,&memory);
    auto allocate=[&](VkMemoryRequirements r,VkMemoryPropertyFlags flags){uint32_t i=0;while(i<memory.memoryTypeCount && (!(r.memoryTypeBits & (1u<<i)) || (memory.memoryTypes[i].propertyFlags & flags)!=flags))i++;CHECK(i<memory.memoryTypeCount);VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=r.size;ai.memoryTypeIndex=i;VkDeviceMemory m{};CHECK(vkAllocateMemory(device,&ai,nullptr,&m)==VK_SUCCESS);return m;};
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ii.imageType=VK_IMAGE_TYPE_2D;ii.format=VK_FORMAT_R8_UNORM;ii.extent={16,16,1};ii.mipLevels=ii.arrayLayers=1;ii.samples=VK_SAMPLE_COUNT_1_BIT;ii.tiling=VK_IMAGE_TILING_OPTIMAL;ii.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;VkImage image{};CHECK(vkCreateImage(device,&ii,nullptr,&image)==VK_SUCCESS);
    VkMemoryRequirements r{};vkGetImageMemoryRequirements(device,image,&r);auto im=allocate(r,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);CHECK(vkBindImageMemory(device,image,im,0)==VK_SUCCESS);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=256;bi.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT;VkBuffer buffer{};CHECK(vkCreateBuffer(device,&bi,nullptr,&buffer)==VK_SUCCESS);vkGetBufferMemoryRequirements(device,buffer,&r);auto bm=allocate(r,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);CHECK(vkBindBufferMemory(device,buffer,bm,0)==VK_SUCCESS);void* map{};CHECK(vkMapMemory(device,bm,0,VK_WHOLE_SIZE,0,&map)==VK_SUCCESS);std::memset(map,127,256);vkUnmapMemory(device,bm);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pci.queueFamilyIndex=family;VkCommandPool pool{};CHECK(vkCreateCommandPool(device,&pci,nullptr,&pool)==VK_SUCCESS);VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cai.commandPool=pool;cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cai.commandBufferCount=1;VkCommandBuffer cmd{};CHECK(vkAllocateCommandBuffers(device,&cai,&cmd)==VK_SUCCESS);VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};CHECK(vkBeginCommandBuffer(cmd,&begin)==VK_SUCCESS);
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};b.srcStageMask=b.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;b.dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b.newLayout=VK_IMAGE_LAYOUT_GENERAL;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,VK_REMAINING_MIP_LEVELS,0,VK_REMAINING_ARRAY_LAYERS};VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};di.imageMemoryBarrierCount=1;di.pImageMemoryBarriers=&b;vkCmdPipelineBarrier2(cmd,&di);
    VkClearColorValue zero{};vkCmdClearColorImage(cmd,image,VK_IMAGE_LAYOUT_GENERAL,&zero,1,&b.subresourceRange);
    if(fixed){b.srcStageMask=VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;b.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;b.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;b.dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;b.oldLayout=b.newLayout=VK_IMAGE_LAYOUT_GENERAL;vkCmdPipelineBarrier2(cmd,&di);}
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={16,16,1};vkCmdCopyBufferToImage(cmd,buffer,image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
    CHECK(vkEndCommandBuffer(cmd)==VK_SUCCESS);VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE)==VK_SUCCESS);CHECK(vkQueueWaitIdle(queue)==VK_SUCCESS);
    vkDestroyCommandPool(device,pool,nullptr);vkDestroyBuffer(device,buffer,nullptr);vkFreeMemory(device,bm,nullptr);vkDestroyImage(device,image,nullptr);vkFreeMemory(device,im,nullptr);vkDestroyDevice(device,nullptr);destroyDebug(instance,messenger,nullptr);vkDestroyInstance(instance,nullptr);FreeLibrary(loader);
    std::printf("validation_errors=%d WAW_hazards=%d\n",errors,hazards);CHECK(fixed ? errors==0 : hazards>0);puts("PASS: API dependency probe expectation met");
}
