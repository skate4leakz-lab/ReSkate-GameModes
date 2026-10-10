#pragma once
#include <array>
#include <cmath>

namespace dingosdk::multiplayer::one_up {
using Facing = std::array<float,9>;
inline constexpr Facing default_facing{1,0,0,0,1,0,0,0,1};
inline bool valid_facing(const Facing& facing) {
    for(float n:facing)if(!std::isfinite(n) || std::abs(n)>1.01f)return false;
    for(unsigned row=0;row<3;++row) {
        float length{};for(unsigned i=0;i<3;++i)length+=facing[row*3+i]*facing[row*3+i];
        if(std::abs(length-1.f)>.03f)return false;
        for(unsigned other=0;other<row;++other) {
            float dot{};for(unsigned i=0;i<3;++i)dot+=facing[row*3+i]*facing[other*3+i];
            if(std::abs(dot)>.03f)return false;
        }
    }
    const auto determinant=facing[0]*(facing[4]*facing[8]-facing[5]*facing[7])-
        facing[1]*(facing[3]*facing[8]-facing[5]*facing[6])+facing[2]*(facing[3]*facing[7]-facing[4]*facing[6]);
    return determinant>.97f;
}
inline std::array<float,16> spawn_transform(const std::array<float,3>& position,const Facing& facing) {
    return {facing[0],facing[1],facing[2],0,facing[3],facing[4],facing[5],0,
        facing[6],facing[7],facing[8],0,position[0],position[1],position[2],1};
}
inline Facing transform_facing(const std::array<float,16>& transform) {
    return {transform[0],transform[1],transform[2],transform[4],transform[5],transform[6],transform[8],transform[9],transform[10]};
}
}
