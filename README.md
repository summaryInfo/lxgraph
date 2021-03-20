LxGraph 
=======

This is a prototype of source code
graph generator developed as a part of
course work. The name is temporary.

It is trying to generate a nice graph
in a graphviz dot format.

At the moment it is possible to exclude
parts of the graph via confiuration either
via specifing different roots or adding
them to exclude lists.

Duplicate edges are collapsed to clean-up
the graph and turned into wider edges.

It is still generates quite messy graphs
for the large code bases and probably needs
custom layout engine that takes into account
modules and files.
The idea is to lay out graphs from to to bottom
laying out file first and then laying out functions
within each file.

## Building and dependencies

The only direct dependency is `libclang` (tested with gcc 10 and libclang 11).
You also need to have graphviz installed to render the graph.

To build:

    make -j

## Running

To see available configuration options run `./lxgraph -h`

### Linux kernel

To run it on linux kernel, you first need to compile the kernel (tested with version 5.4)
and generate `compile_commands.json` with `scripts/gen_compile_commands.py`
shipped with the kernel. Then you can run this generator:

    cd /path/to/the/kernel
    make defconfig
    make -j$(nproc)
    ./scripts/gen_compile_commands.py
    cd -
    ./lxgraph -p /path/to/the/kernel -C contrib/lxgraph.linux.conf
    dot -Tsvg graph.dot > graph.svg

### NSST

To run it on other programs you also need to generate `compile_commands.json`.
This can be done for example with [Bear](https://github.com/rizsotto/Bear).
The example generation for [nsst termintal](https://github.com/summaryInfo/nsst):

    cd /path/to/nsst
    bear make force -j
    cd -
    ./lxgraph -p /path/to/nsst -C contrib/lxgraph.nsst.conf
    dot -Tsvg graph.dot > graph.svg

## TODO

* Support configuring files-to-modules correspondance and
  take is into account while generating the graph.

* Implement custom layout engine.

* Generate links to the parts of the graph to make it more navigatable.

* Remove non-essential parts to reduce the noise

* Add option to remove static/inline functions from the graph

