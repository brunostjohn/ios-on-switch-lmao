#include <metal_stdlib>
using namespace metal;

// tests/PredicationTest.swift: the GPU writes every predicate word itself, and each predicated dispatch leaves its mark
kernel void predication_store(device uint *words [[buffer(0)]], constant uint2 &at [[buffer(1)]])
{
    words[at.x] = at.y;
}

kernel void predication_mark(device uint *marks [[buffer(0)]], constant uint2 &at [[buffer(1)]])
{
    marks[at.x] = at.y;
}
