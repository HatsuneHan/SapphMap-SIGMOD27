#pragma once
#include "oram.hpp"
#include "types.hpp"

struct Edge
{
    int to, next;
    double weight;

    Edge() : to(-1), next(-1), weight(0) {}
};

ORAM::ObliviousRAM<uint32_t, int> init_head(int n);
ORAM::ObliviousRAM<uint32_t, Edge> init_edge(std::vector<Edge> &edges_);
ORAM::ObliviousRAM<uint32_t, Edge> init_edge(int m);
void add_edge(std::vector<Edge> &edges,
              ORAM::ObliviousRAM<uint32_t, int> &head,
              int source, int to, int weight, int idx);
void add_edge(ORAM::ObliviousRAM<uint32_t, Edge> &edges,
              ORAM::ObliviousRAM<uint32_t, int> &head,
              int from, int to, int cnt);

ORAM::ObliviousRAM<uint32_t, int> shortest_path(const ORAM::ObliviousRAM<uint32_t, Edge> &edges,
                                                const ORAM::ObliviousRAM<uint32_t, int> &head,
                                                int from);

std::vector<int> dijkstra(const std::vector<std::vector<int>> &graph, int from);
