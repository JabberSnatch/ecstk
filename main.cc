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

template <typename Tuple, uint32_t index>
void TupleAssignment(Tuple& tuple, void** args)
{}

template <typename Tuple, uint32_t index, typename T, typename ... Pack>
void TupleAssignment(Tuple& tuple, void** args)
{
    std::get<index>(tuple) = *(T*)*args;
    TupleAssignment<Tuple, index+1, Pack...>(tuple, args+1);
}


template <typename Tuple, typename Func, typename T, T ...ints>
void ForwardArguments(Func func, void** args, std::integer_sequence<T, ints...>)
{
    Tuple tuple{};
    //int dummy[sizeof...(ints)] = {( tuple.get<ints>() = args
    //func(*(std::tuple_element<ints, Tuple>::type*)(args)...);
    func(std::get<ints>(tuple)...);
         //args[ints]...);
}

template <typename Tuple, typename Func, typename T, T ...ints>
void ForwardTuple(Func func, Tuple const& args, std::integer_sequence<T, ints...>)
{
    func(std::get<ints>(args)...);
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
        using IndexSequence = std::index_sequence_for<Components...>;

        std::array<ComponentID, sizeof...(Components)> tags = { Component<Components>()... };
        std::array<ComponentStorage const*, sizeof...(Components)> storages = {
            &components[Component<Components>()]...
        };

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
                std::array<void*, sizeof...(Components)> components = {};

                for (size_t tag_index = 0; tag_index < components.size(); ++tag_index)
                {
                    ComponentID tag = tags[tag_index];
                    ComponentBinding const& binding =
                        *std::find_if(bindings.begin(), bindings.end(),
                                      [tag](ComponentBinding const& binding){
                                          return binding.type == tag;
                                      });

                    ComponentStorage const& storage = *storages[tag_index];
                    components[tag_index] = storage.data + storage.size*binding.tag;
                }

                void** argument = &components[0];

                using Tuple = std::tuple<Components...>;
                Tuple tuple{};
                TupleAssignment<Tuple, 0, Components...>(tuple, argument);
                //ForwardArguments<std::tuple<Components...>>(callback, argument, IndexSequence{});
                ForwardTuple(callback, tuple, IndexSequence{});

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
            world.BindComponent(entity, Transform{ 0.f + (float)index, 100.f, 200.f });
            world.BindComponent(entity, RenderData{ (int)index+5 });
        }

        {
            EntityTag entity = world.SpawnEntity();
            world.BindComponent(entity, RenderData{ 4 });
        }
    }

    world.RunSystem<Transform, RenderData>(
        [](Transform const& transform, RenderData const& renderData){
            std::cout << transform.x << " " << transform.y << " " << transform.z << std::endl;
        }
    );

    return 0;
}
