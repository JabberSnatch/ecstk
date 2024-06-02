#include <functional>
#include <vector>
#include <unordered_map>
#include <cstring>
#include <type_traits>
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
static ComponentID ComponentImpl() {
    static ComponentID const tag = DeclareComponent();
    return tag;
}

template <typename T>
static ComponentID Component() {
    return ComponentImpl<typename std::remove_cvref<T>::type>();
}

template <typename T>
using TupleType =
    typename std::add_pointer<typename std::remove_reference<T>::type>::type;

template <typename Tuple, uint32_t index>
void TupleAssignment(Tuple& tuple, void** args)
{}

template <typename Tuple, uint32_t index, typename T, typename ... Pack>
void TupleAssignment(Tuple& tuple, void** args)
{
    std::get<index>(tuple) = (T*)*args;
    TupleAssignment<Tuple, index+1, Pack...>(tuple, args+1);
}

template <typename Tuple, typename Func, typename T, T ...ints>
void ForwardTuple(Func func, Tuple const& args, std::integer_sequence<T, ints...>)
{
    func(*std::get<ints>(args)...);
}

template <typename ... CTypes>
void ForwardFunc(auto callback, void** arguments)
{
    using IndexSequence = std::index_sequence_for<CTypes...>;
    using Tuple = std::tuple<TupleType<CTypes>...>;
    Tuple tuple{};
    TupleAssignment<Tuple, 0, std::remove_reference_t<CTypes>...>(tuple, arguments);
    ForwardTuple(callback, tuple, IndexSequence{});
}

struct IndexAllocator
{
    using IndexRange = std::pair<uint64_t, uint64_t>;

    IndexAllocator() : free_ranges{ { 0ull, ~0ull } } {}
    uint64_t ReserveIndex() {
        if (free_ranges.empty()) return ~0ull;
        IndexRange& first_range = free_ranges.back();
        uint64_t index = first_range.first++;
        if (first_range.first == first_range.second)
            free_ranges.pop_back();
        return index;
    }

    void ReleaseIndex(uint64_t index) {
        if (free_ranges.empty()) {
            free_ranges.emplace_back(index, index+1);
            return;
        }

        auto selected_range =
            std::upper_bound(
                free_ranges.rbegin(), free_ranges.rend(),
                index,
                [](uint64_t index, IndexRange const& range) {
                    return index < range.second;
                });

        auto previous_range = selected_range-1;
        bool fits_in_previous = (
            selected_range != free_ranges.rbegin()
            && previous_range->second == index);
        bool fits_in_selected = (
            selected_range != free_ranges.rend()
            && selected_range->first == index+1);

        if (!fits_in_previous && !fits_in_selected)
        {
            free_ranges.insert(selected_range.base(), IndexRange{ index, index+1 });
            return;
        }

        if (fits_in_previous && fits_in_selected)
        {
            selected_range->first = previous_range->first;
            free_ranges.erase(selected_range.base());
            return;
        }

        if (fits_in_previous)
            ++previous_range->second;

        if (fits_in_selected)
            --selected_range->first;
    }

    std::vector<IndexRange> free_ranges;
};

struct World
{
    EntityTag SpawnEntity();
    template <typename CType> void BindComponent(EntityTag e, CType const& c);
    template <typename ... CTypes> void RunSystem(auto callback);

    void Commit();

    struct state_t
    {
        EntityTag next_entity;
        std::vector<std::vector<ComponentBinding>> entities;
        std::unordered_map<ComponentID, ComponentStorage> components;
    };

    state_t current_state;
    state_t pending_state;

    EntityTag next_entity;
    std::vector<std::vector<ComponentBinding>> entities;
    std::unordered_map<ComponentID, ComponentStorage> components;
};

EntityTag World::SpawnEntity()
{
    EntityTag tag = entities.size();
    entities.emplace_back();
    return tag;
}

template <typename CType>
void World::BindComponent(EntityTag e, CType const& c)
{
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

template <typename ... CTypes>
void World::RunSystem(auto callback)
{
    static const std::array<ComponentID, sizeof...(CTypes)> tags =
        { Component<CTypes>()... };

    std::array<ComponentStorage const*, sizeof...(CTypes)> storages = {
        &components[Component<CTypes>()]...
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
            std::array<void*, sizeof...(CTypes)> components = {};

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

            ForwardFunc<CTypes...>(callback, components.data());
        }
    }
}

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
    IndexAllocator test{};
    for (uint32_t index = 0; index < 100; ++index)
        test.ReserveIndex();

    test.ReleaseIndex(50);
    test.ReleaseIndex(99);
    test.ReleaseIndex(48);
    test.ReleaseIndex(49);
    test.ReleaseIndex(47);

    for (uint32_t index = 0; index < 4; ++index)
        test.ReserveIndex();

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
            std::cout << "(" << transform.x << " " << transform.y << " " << transform.z << ") ";
        }
    );
    std::cout << std::endl << std::endl;

    world.RunSystem<RenderData, Transform>(
        [](RenderData& renderData, Transform& transform){
            std::cout << "(" << transform.x << " " << renderData.mesh_index << ") ";
            transform.x *= 2.f;
        }
    );
    std::cout << std::endl << std::endl;

    world.RunSystem<Transform const>(
        [](Transform const& transform) {
            std::cout << transform.x << " ";
        }
    );
    std::cout << std::endl << std::endl;

    world.RunSystem<RenderData const>(
        [](RenderData const& renderData) {
            std::cout << renderData.mesh_index << " ";
        }
    );
    std::cout << std::endl << std::endl;

    return 0;
}
