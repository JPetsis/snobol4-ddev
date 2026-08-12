<?php

declare(strict_types=1);

namespace Snobol\Tests;

use PHPUnit\Framework\TestCase;
use Snobol\Array_;
use Snobol\Builder;
use Snobol\DynamicPatternCache;
use Snobol\Pattern;
use Snobol\PatternCache;
use Snobol\Table;

/**
 * Validation and arginfo alignment tests (fix-php-binding-scan-findings
 * group 5): Builder register bounds, repeat(null) max, nullable cache and
 * table constructors, and Array_ int32 range checks.
 */
final class ValidationTest extends TestCase
{
    public function testBuilderCapRejectsOutOfRangeRegisters(): void
    {
        $this->assertIsArray(Builder::cap(0, Builder::lit('a')));
        $this->assertIsArray(Builder::cap(63, Builder::lit('a')));

        $this->expectException(\ValueError::class);
        Builder::cap(-1, Builder::lit('a'));
    }

    public function testBuilderCapRejectsRegister64(): void
    {
        $this->expectException(\ValueError::class);
        Builder::cap(64, Builder::lit('a'));
    }

    public function testBuilderAssignValidatesBothRegisters(): void
    {
        $this->assertIsArray(Builder::assign(0, 0));

        try {
            Builder::assign(64, 0);
            $this->fail('assign(64, 0) must throw');
        } catch (\ValueError) {
        }

        try {
            Builder::assign(0, -1);
            $this->fail('assign(0, -1) must throw');
        } catch (\ValueError) {
        }
    }

    public function testBuilderEvalValidatesRegister(): void
    {
        $this->assertIsArray(Builder::eval(0, 0));
        $this->expectException(\ValueError::class);
        Builder::eval(0, 64);
    }

    public function testBuilderEmitRefValidatesRegister(): void
    {
        $this->assertIsArray(Builder::emitRef(0));
        $this->expectException(\ValueError::class);
        Builder::emitRef(-1);
    }

    public function testBuilderRepeatAcceptsNullMax(): void
    {
        $node = Builder::repeat(Builder::lit('a'), 0, null);
        $this->assertIsArray($node);
        $this->assertSame(-1, $node['max']); // unbounded default

        // The resulting pattern compiles and matches.
        $p = Pattern::compileFromAst($node);
        $this->assertIsArray($p->match('aaa'));
    }

    public function testDynamicPatternCacheNullCapacity(): void
    {
        $cache = new DynamicPatternCache(null);
        $this->assertSame(128, $cache->stats()['max_size']);
    }

    public function testPatternCacheNullCapacity(): void
    {
        $cache = new PatternCache(null);
        $cache->get('k', fn () => 'v');
        $this->assertSame(1, $cache->size());
    }

    public function testTableNullName(): void
    {
        $table = new Table(null);
        $table->set('k', 'v');
        $this->assertSame('v', $table->get('k'));
    }

    public function testArrayConstructorRejectsNegativeSize(): void
    {
        $this->expectException(\ValueError::class);
        new Array_(-1);
    }

    public function testArrayConstructorRejectsHugeSize(): void
    {
        $this->expectException(\ValueError::class);
        new Array_(PHP_INT_MAX);
    }

    public function testArraySetRejectsOutOfInt32Key(): void
    {
        $arr = new Array_(4);
        $this->expectException(\ValueError::class);
        $arr->set(2147483648, 'v');
    }

    public function testArrayGetRejectsOutOfInt32Key(): void
    {
        $arr = new Array_(4);
        $this->expectException(\ValueError::class);
        $arr->get(-2147483649);
    }
}
