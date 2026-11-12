#include "core/channel.h"
#include "core/sched.h"

namespace Rudracore::Core {

u64 Task::addHandle(Object* object) {
    for (u64 h = 1; h < MAX_HANDLES; h++) {
        if (not handles[h]) {
            handles[h] = object;
            return h;
        }
    }
    return 0;
}

Object* Task::handle(u64 h) const {
    if (h == 0 or h >= MAX_HANDLES)
        return nullptr;
    return handles[h];
}

Object* Task::takeHandle(u64 h) {
    Object* object = handle(h);
    if (object)
        handles[h] = nullptr;
    return object;
}

void Task::releaseResources() {
    for (auto& object : handles) {
        if (object) {
            // An endpoint may outlive this handle (another reference holds
            // it): it must not keep pointing at this task as its waiter.
            if (object->type == ObjectType::Channel) {
                auto* endpoint = static_cast<Endpoint*>(object);
                if (endpoint->waiter == this)
                    endpoint->waiter = nullptr;
            }
            object->unref();
            object = nullptr;
        }
    }

    // Tear down the address space before dropping the mapped objects, so
    // their memory is never still mapped when it gets freed.
    if (space) {
        Vmm::destroyUserSpace(space);
        space = nullptr;
    }

    for (auto& mapping : mappings) {
        if (mapping.object) {
            mapping.object->unref();
            mapping = {};
        }
    }

    if (quota) {
        quota->unref();
        quota = nullptr;
    }
}

} // namespace Rudracore::Core
