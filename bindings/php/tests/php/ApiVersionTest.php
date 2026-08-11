<?php

namespace Snobol\Tests;

use PHPUnit\Framework\TestCase;

/**
 * PHP test for snobol_get_api_version().
 *
 * Verifies the version encoding (MAJOR << 16 | MINOR << 8 | PATCH)
 * and that the major component matches SNOBOL_VERSION_MAJOR = 1.
 */
class ApiVersionTest extends TestCase
{
    public function testFunctionExists(): void
    {
        $this->assertTrue(
            function_exists('snobol_get_api_version'),
            'snobol_get_api_version() must be available'
        );
    }

    public function testReturnsInteger(): void
    {
        $v = snobol_get_api_version();
        $this->assertIsInt($v);
    }

    public function testMajorVersionIsOne(): void
    {
        $v = snobol_get_api_version();
        $major = ($v >> 16) & 0xFF;
        $this->assertSame(1, $major, 'Major version component must be 1');
    }

    public function testMinorVersionIsZero(): void
    {
        $v = snobol_get_api_version();
        $minor = ($v >> 8) & 0xFF;
        $this->assertSame(0, $minor, 'Minor version component must be 0 (v1.0.0)');
    }

    public function testEncodingMatchesV104(): void
    {
        // v1.0.4 encodes as (1 << 16) | (0 << 8) | 4 = 0x00010004
        $expected = (1 << 16) | (0 << 8) | 4;
        $this->assertSame($expected, snobol_get_api_version());
    }

    public function testAbiVersionFunctionExists(): void
    {
        $this->assertTrue(
            function_exists('snobol_get_abi_version'),
            'snobol_get_abi_version() must be available'
        );
    }

    public function testAbiVersionReturnsOne(): void
    {
        $v = snobol_get_abi_version();
        $this->assertSame(1, $v, 'Initial ABI version must be 1');
    }

    public function testChoiceStatsShape(): void
    {
        $stats = snobol_get_choice_stats();
        $this->assertIsArray($stats);
        $this->assertArrayHasKey('choice_push_count', $stats);
        $this->assertArrayHasKey('choice_allocated', $stats);
        $this->assertArrayHasKey('choice_stack_depth', $stats);
        $this->assertArrayHasKey('choice_stack_memory_usage', $stats);
    }

    public function testPhpInfoReportsModule(): void
    {
        // MINFO branch: module table rendered into the captured output
        ob_start();
        phpinfo(INFO_MODULES);
        $output = ob_get_clean();
        $this->assertIsString($output);
        $this->assertStringContainsString('snobol support', $output);
    }

    public function testExtraArgumentsRejected(): void
    {
        // Zero-arg functions reject extra arguments before entering the body
        foreach (['snobol_get_api_version', 'snobol_get_abi_version', 'snobol_get_choice_stats'] as $fn) {
            try {
                $fn(1);
                $this->fail("Expected ArgumentCountError from {$fn}(1)");
            } catch (\ArgumentCountError $e) {
                $this->assertTrue(true);
            }
        }
    }
}

