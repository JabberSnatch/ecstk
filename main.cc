#include <functional>

using Entity = uint64_t;

struct World
{
    Entity SpawnEntity()
    {
        return {};
    }

    template <typename Component>
    void BindComponent(Entity e, Component const& c)
    {}

    template <typename ... Components>
    void RunSystem(auto callback)
    {}

    Entity next_id;
    std::vector<Entity> free_ids;
};


struct Transform
{
    int a;
};

struct RenderData
{
    int b;
};

int main(int argc, char const** argv)
{
    World world{};
    world.RunSystem<Transform const&, RenderData const&>(
        [](Transform const&, RenderData const&){
        }
    );

    return 0;
}
