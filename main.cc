#include <functional>
#include <vector>
#include <unordered_map>
#include <cstring>
#include <type_traits>
#include <memory>
#include <iostream>
#include <cmath>

struct IndexAllocator
{
    using IndexRange = std::pair<uint64_t, uint64_t>;

    IndexAllocator() : free_ranges{ { 0ull, ~0ull } } {}

    uint64_t IndexCount() const { return free_ranges[0].first; }

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

struct BlockAllocator
{
    void* operator[](uint64_t index) const {
        return data[index/block_size].get() + (index%block_size)*element_size;
    }

    void NewBlock() {
        data.emplace_back(new uint8_t[element_size * block_size]);
    }

    size_t StorageSize() const {
        return data.size() * block_size;
    }

    uint64_t FindIndex(void const* element) const {
        for (uint32_t block_index = 0; block_index < data.size(); ++block_index)
        {
            ptrdiff_t distance = (uint8_t const*)element - data[block_index].get();
            if (distance >= 0 && distance < (ptrdiff_t)(block_size*element_size))
                return block_index * block_size + (distance / element_size);
        }
        return ~0ull;
    }

    size_t element_size;
    size_t block_size;
    std::vector<std::unique_ptr<uint8_t[]>> data;
};

using EntityTag = uint64_t;
using ComponentID = uint64_t;
using ComponentTag = uint64_t;

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

struct World
{
    EntityTag SpawnEntity();
    void KillEntity(EntityTag e);
    template <typename CType> void BindComponent(EntityTag e, CType const& c);
    template <typename ... CTypes> void RunSystem(auto callback);

    template <typename CType> EntityTag EntityLookup(CType const& c);
    template <typename CType> CType* ComponentLookup(EntityTag entity);

    void Commit();

    struct ComponentBinding {
        ComponentID type;
        ComponentTag tag;
    };

    struct ComponentStorage {
        IndexAllocator indices;
        BlockAllocator data;
        BlockAllocator entity_bindings;
    };

    struct ComponentBindingDesc {
        EntityTag entity;
        ComponentBinding binding;
    };

    IndexAllocator entity_indices;
    std::vector<ComponentBindingDesc> pending_bindings;
    std::vector<EntityTag> pending_deletions;

    std::vector<std::vector<ComponentBinding>> entities;
    std::unordered_map<ComponentID, ComponentStorage> components;
};

EntityTag World::SpawnEntity()
{
    return entity_indices.ReserveIndex();
}

void World::KillEntity(EntityTag e)
{
    pending_deletions.push_back(e);
}

template <typename CType>
void World::BindComponent(EntityTag e, CType const& c)
{
    ComponentID const component_id = Component<CType>();

    if (components.count(component_id) == 0)
    {
        components.emplace(component_id, ComponentStorage{});
        ComponentStorage& storage = components[component_id];
        storage.data.element_size = sizeof(CType);
        storage.data.block_size = 256;
        storage.entity_bindings.element_size = sizeof(uint64_t);
        storage.entity_bindings.block_size = 2048;
    }

    ComponentStorage& storage = components[component_id];
    ComponentTag component_tag = storage.indices.ReserveIndex();
    if (storage.indices.IndexCount() > storage.data.StorageSize())
    {
        storage.data.NewBlock();
        storage.entity_bindings.NewBlock();
    }

    std::memcpy(storage.data[component_tag], &c, storage.data.element_size);
    *(uint64_t*)storage.entity_bindings[component_tag] = e;

    pending_bindings.emplace_back(ComponentBindingDesc{ e, { component_id, component_tag } });
}

template <typename CType>
EntityTag World::EntityLookup(CType const& c)
{
    ComponentID const component_id = Component<CType>();

    ComponentStorage const& storage = components[component_id];
    uint64_t component_index = storage.data.FindIndex(&c);
    return *(uint64_t*)storage.entity_bindings[component_index];
}

void World::Commit()
{
    entities.resize(entity_indices.IndexCount());
    for (ComponentBindingDesc& binding : pending_bindings)
        entities[binding.entity].emplace_back(std::move(binding.binding));
    pending_bindings.clear();

    for (EntityTag entity : pending_deletions)
    {
        for (ComponentBinding const& binding : entities[entity])
            components[binding.type].indices.ReleaseIndex(binding.tag);
        entities[entity].clear();
        entity_indices.ReleaseIndex(entity);
    }
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
                components[tag_index] = storage.data[binding.tag];
            }

            ForwardFunc<CTypes...>(callback, components.data());
        }
    }
}

struct Boid
{
    float px, py;
    float vx, vy;
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
        EntityTag entity = world.SpawnEntity();
        Boid boid = {};
        boid.px = (float)(index / 10);
        boid.py = (float)(index % 10);
        boid.vx = 0.5f;
        boid.vy = 0.f;
        world.BindComponent(entity, boid);
    }

    #if 0
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
    #endif

    world.Commit();

    static auto const boids_update = [&](){
        float dt = 0.016f;
        world.RunSystem<Boid>(
            [&world, dt](Boid& boid) {
                EntityTag entity = world.EntityLookup(boid);

                uint32_t neighbour_count = 0;
                float radius = 2.f;
                float cx = 0.f, cy = 0.f;
                float sx = 0.f, sy = 0.f;
                float ax = 0.f, ay = 0.f;
                world.RunSystem<Boid const>(
                    [&](Boid const& other_boid) {
                        EntityTag other_entity = world.EntityLookup(other_boid);
                        if (other_entity == entity)
                            return;

                        float dx = boid.px - other_boid.px;
                        float dy = boid.py - other_boid.py;
                        float distance = std::sqrt(dx*dx + dy*dy);
                        if (distance > radius)
                            return;

                        ++neighbour_count;

                        if (distance > 0.001f)
                        {
                            sx += dx / distance;
                            sy += dy / distance;
                        }

                        cx += other_boid.px;
                        cy += other_boid.py;

                        ax += other_boid.vx;
                        ay += other_boid.vy;
                    }
                );

                if (!neighbour_count)
                    return;

                cx /= (float)neighbour_count;
                cy /= (float)neighbour_count;

                sx /= (float)neighbour_count;
                sy /= (float)neighbour_count;

                ax /= (float)neighbour_count;
                ay /= (float)neighbour_count;

                boid.vx += (cx + sx + ax) / 3.f;
                boid.vy += (cy + sy + ay) / 3.f;

                boid.px += boid.vx * dt;
                boid.py += boid.vy * dt;

                std::cout << "(" << boid.px << " " << boid.py << ") ";
            }
        );
    };

    for (uint32_t index = 0; index < 100; ++index)
    {
        boids_update();
        std::cout << "completed boids update " << index << std::endl;
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
        [&world](Transform const& transform) {
            EntityTag source_entity = world.EntityLookup(transform);
            std::cout << "(" << transform.x << " " << source_entity << ") ";
            EntityTag entity = world.SpawnEntity();
            world.BindComponent(entity, transform);
            world.KillEntity(source_entity);
        }
    );
    std::cout << std::endl << std::endl;

    world.Commit();

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
