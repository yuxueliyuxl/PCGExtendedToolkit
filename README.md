# PCG Extended Toolkit 

![PCGEx](/Resources/Icon128.png)

# What is it?
 The PCG Extended Toolkit is a plugin for [Unreal engine 5](https://www.unrealengine.com/en-US/) (5.3.x) that contains a collection of **low-level PCG Graph elements** offering additional ways to manipulate and control PCG Data.

While there are a bunch of misc (useful!) nodes in this toolkit, its primary goal revolves around manipulating relationships between points through graphs (Delaunay, Voronoi, Minimum Spanning Tree etc); and then extract more concrete informations from these networks to, for example, build splines, paths, general flow-related creative stuffs that may otherwise be harder to achieve.

## PCGEx Nodes
Generally speaking, PCGEx nodes are not "magic". They aim to enhance existing features in a highly modular way, allowing content creators to achieve more without making assumptions about their specific goals. In other words, PCGEx' nodes do very little on their own.  
Check out the [Full documentation](https://nebukam.github.io/pcgextendedtoolkit/)!

*Note: Most of the nodes are multithreaded or async!*

## Getting Started
* [Installation](https://nebukam.github.io/pcgextendedtoolkit/installation/)
* [All Nodes](https://nebukam.github.io/pcgextendedtoolkit/nodes/)
* [Examples & Guides](https://nebukam.github.io/pcgextendedtoolkit/guides/)

### Thanks
- Kudo to [MikeC] for his reckless experiment with uncooked releases, feedbacks, suggestions. Without him this plugin wouldn't be as useful and stable as it is today.

### Footnotes
- 3D Delaunay adapted from the excellent [Scrawk' Hull-Delaunay-Voronoi](https://github.com/Scrawk/Hull-Delaunay-Voronoi) repo
