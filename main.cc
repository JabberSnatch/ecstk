#include <functional>
#include <vector>
#include <unordered_map>
#include <cstring>
#include <iostream>

using EntityTag = uint64_t;
using ComponentID = uint64_t;
using ComponentTag = uint64_t;

struct ComponentBinding {
    ComponentID type;
    ComponentTag tag;
};

struct ComponentStorage {
    size_t size;
    size_t count;
    size_t alloc_count;
    uint8_t* data;
};

static ComponentID DeclareComponent() {
    static ComponentID next_component = 0;
    return next_component++;
};

template <typename T>
static ComponentID Component() {
    static ComponentID const tag = DeclareComponent();
    return tag;
}

struct World
{
    EntityTag SpawnEntity() {
        EntityTag tag = entities.size();
        entities.emplace_back();
        return tag;
    }

    template <typename CType>
    void BindComponent(EntityTag e, CType const& c) {
        ComponentID component_id = Component<CType>();

        if (components.count(component_id) == 0)
        {
            components.emplace(component_id, ComponentStorage{});
            ComponentStorage& storage = components[component_id];
            storage.size = sizeof(CType);
            storage.count = 0;
            storage.alloc_count = 8;
            storage.data = new uint8_t[storage.alloc_count * storage.size];
        }

        ComponentStorage& storage = components[component_id];
        ComponentTag component_tag = storage.count++;
        if (storage.count > storage.alloc_count)
        {
            storage.alloc_count *= 2;
            uint8_t* data = new uint8_t[storage.alloc_count * storage.size];
            std::memcpy(data, storage.data, (storage.count-1) * storage.size);
            delete [] storage.data;
            storage.data = data;
        }

        std::memcpy(storage.data + component_tag*storage.size, &c, storage.size);

        entities[e].emplace_back(ComponentBinding{ component_id, component_tag });
    }

    template <typename ... Components>
    void RunSystem(auto callback)
    {
        std::vector<ComponentID> tags = { Component<Components>()... };
        for (size_t entity_index = 0; entity_index < entities.size(); ++entity_index)
        {
            auto const& bindings = entities[entity_index];
            if (std::all_of(tags.begin(), tags.end(), [&bindings](ComponentID tag) {
                return std::find_if(bindings.begin(), bindings.end(),
                                    [tag](ComponentBinding const& binding){
                                        return binding.type == tag;
                                    })
                    != bindings.end();
            }))
            {
                std::cout << "match found " << entity_index << std::endl;
            }
        }
    }

    EntityTag next_entity;

    std::vector<std::vector<ComponentBinding>> entities;
    std::unordered_map<ComponentID, ComponentStorage> components;
};


struct Transform
{
    float x;
    float y;
    float z;
};

struct RenderData
{
    int mesh_index;
};

int main(int argc, char const** argv)
{
    World world{};

    for (uint32_t index = 0; index < 100; ++index)
    {
        {
            EntityTag entity = world.SpawnEntity();
            world.BindComponent(entity, Transform{ 0.f, 1.f, 2.f });
            world.BindComponent(entity, RenderData{ 4 });
        }

        {
            EntityTag entity = world.SpawnEntity();
            world.BindComponent(entity, RenderData{ 4 });
        }
    }

    world.RunSystem<Transform, RenderData>(
        [](Transform const&, RenderData const&){
        }
    );

    return 0;
}
