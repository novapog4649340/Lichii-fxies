// SPDX-License-Identifier: GPL-3.0-or-later

// Mesh programs whose SET_MESH_SHADER_A output topology is points (0) or lines (1): Ocarina of
// Time's garden hedges use topology 0, and their pipelines failed to build while only triangles
// were implemented.

#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/abstract_syntax_list.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/object_pool.h"

namespace {
// SPIR-V opcodes, execution modes and built-ins the mesh emission is checked against, from
// spirv/unified1/spirv.h.
constexpr u32 OpExecutionMode = 16;
constexpr u32 OpDecorate = 71;
constexpr u32 DecorationBuiltIn = 11;
constexpr u32 ExecutionModeOutputPoints = 27;
constexpr u32 ExecutionModeOutputLinesEXT = 5269;
constexpr u32 ExecutionModeOutputTrianglesEXT = 5298;
constexpr u32 BuiltInPointSize = 1;
constexpr u32 BuiltInPrimitivePointIndicesEXT = 5294;
constexpr u32 BuiltInPrimitiveLineIndicesEXT = 5295;
constexpr u32 BuiltInPrimitiveTriangleIndicesEXT = 5296;

struct Instruction {
    const std::vector<u32>& code;
    size_t offset;

    [[nodiscard]] u32 Opcode() const {
        return code[offset] & 0xffff;
    }
    [[nodiscard]] u32 Operand(size_t index) const {
        return code[offset + index];
    }
};

/// Every instruction of the module, in order.
std::vector<Instruction> Instructions(const std::vector<u32>& code) {
    std::vector<Instruction> instructions;
    for (size_t offset = 5; offset < code.size();) {
        const u32 word_count{code[offset] >> 16};
        if (word_count == 0) {
            break;
        }
        instructions.push_back({code, offset});
        offset += word_count;
    }
    return instructions;
}

bool HasExecutionMode(const std::vector<u32>& code, u32 mode) {
    for (const Instruction& instruction : Instructions(code)) {
        if (instruction.Opcode() == OpExecutionMode && instruction.Operand(2) == mode) {
            return true;
        }
    }
    return false;
}

bool HasBuiltIn(const std::vector<u32>& code, u32 builtin) {
    for (const Instruction& instruction : Instructions(code)) {
        if (instruction.Opcode() == OpDecorate && instruction.Operand(2) == DecorationBuiltIn &&
            instruction.Operand(3) == builtin) {
            return true;
        }
    }
    return false;
}

std::vector<u32> Emit(u32 topology) {
    Shader::ObjectPool<Shader::IR::Inst> inst_pool;
    Shader::IR::Block block{inst_pool};
    Shader::IR::Program program;
    program.stage = Shader::Stage::Mesh;
    program.workgroup_size = {32, 1, 1};
    program.mesh.topology = topology;
    program.mesh.max_vertices = 32;
    program.mesh.max_primitives = 16;
    // One vertex line (position) and one primitive line, so the epilogue has something to copy.
    program.mesh.vertex_lines = {static_cast<u32>(Shader::IR::Attribute::PositionX)};
    program.mesh.primitive_lines = {static_cast<u32>(Shader::IR::Attribute::PrimitiveId)};
    // The epilogue addresses its vertices and primitives with the local invocation id.
    program.info.uses_local_invocation_id = true;
    program.blocks.push_back(&block);
    program.post_order_blocks.push_back(&block);
    Shader::IR::IREmitter ir{block};
    ir.Prologue();
    ir.Epilogue();
    Shader::IR::AbstractSyntaxNode block_node{};
    block_node.type = Shader::IR::AbstractSyntaxNode::Type::Block;
    block_node.data.block = &block;
    Shader::IR::AbstractSyntaxNode return_node{};
    return_node.type = Shader::IR::AbstractSyntaxNode::Type::Return;
    program.syntax_list = {block_node, return_node};
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010300;
    return Shader::Backend::SPIRV::EmitSPIRV(profile, program);
}
} // Anonymous namespace

TEST_CASE("Mesh programs output the topology SET_MESH_SHADER_A asks for") {
    const std::vector<u32> triangles{Emit(4)};
    CHECK(HasExecutionMode(triangles, ExecutionModeOutputTrianglesEXT));
    CHECK(HasBuiltIn(triangles, BuiltInPrimitiveTriangleIndicesEXT));
    CHECK_FALSE(HasBuiltIn(triangles, BuiltInPointSize));

    const std::vector<u32> lines{Emit(1)};
    CHECK(HasExecutionMode(lines, ExecutionModeOutputLinesEXT));
    CHECK(HasBuiltIn(lines, BuiltInPrimitiveLineIndicesEXT));
    CHECK_FALSE(HasBuiltIn(lines, BuiltInPointSize));

    const std::vector<u32> points{Emit(0)};
    CHECK(HasExecutionMode(points, ExecutionModeOutputPoints));
    CHECK_FALSE(HasExecutionMode(points, ExecutionModeOutputTrianglesEXT));
    CHECK(HasBuiltIn(points, BuiltInPrimitivePointIndicesEXT));
    CHECK_FALSE(HasBuiltIn(points, BuiltInPrimitiveTriangleIndicesEXT));
    // Vulkan wants a size per vertex for point output.
    CHECK(HasBuiltIn(points, BuiltInPointSize));
}
