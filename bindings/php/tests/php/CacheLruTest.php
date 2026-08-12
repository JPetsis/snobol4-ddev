<?php

declare(strict_types=1);

namespace Snobol\Tests;

use PHPUnit\Framework\TestCase;
use Snobol\DynamicPatternCache;
use Snobol\PatternCache;

/**
 * LRU cache correctness tests (fix-php-binding-scan-findings group 4):
 * eviction must keep the cache at capacity and touch must refresh the
 * access order (regression: the manual index-shift loops corrupted the
 * order array, leaving orphaned entries and holes).
 */
final class CacheLruTest extends TestCase
{
    public function testDynamicCacheRespectsCapacity(): void
    {
        $cache = new DynamicPatternCache(3);

        foreach (['a', 'b', 'c', 'd'] as $src) {
            $cache->compile("'$src'");
        }

        $stats = $cache->stats();
        $this->assertSame(3, $stats['size']);
        $this->assertSame(3, $stats['max_size']);

        // 'a' was evicted; 'd' is still resident.
        $this->assertFalse($cache->compile("'a'")['cached']);
        $this->assertTrue($cache->compile("'d'")['cached']);
    }

    public function testDynamicCacheTouchRefreshesLruOrder(): void
    {
        $cache = new DynamicPatternCache(3);

        $cache->compile("'a'");
        $cache->compile("'b'");
        $cache->compile("'c'");
        $cache->compile("'a'"); // touch: 'a' becomes most-recent
        $cache->compile("'d'"); // evicts 'b' (now least-recent)

        // get() is non-mutating (no eviction on miss), so it can probe
        // presence without changing the cache contents.
        $this->assertFalse($cache->get("'b'")['found']); // evicted
        $this->assertTrue($cache->get("'a'")['found']);
        $this->assertTrue($cache->get("'c'")['found']);
        $this->assertTrue($cache->get("'d'")['found']);
        $this->assertSame(3, $cache->stats()['size']);
    }

    public function testDynamicCacheClear(): void
    {
        $cache = new DynamicPatternCache(3);
        $cache->compile("'a'");
        $cache->compile("'b'");
        $cache->clear();

        $this->assertSame(0, $cache->stats()['size']);
        $this->assertFalse($cache->compile("'a'")['cached']);
    }

    public function testPatternCacheRespectsCapacity(): void
    {
        $cache = new PatternCache(3);
        $factoryCalls = 0;
        $factory = function () use (&$factoryCalls) {
            $factoryCalls++;
            return 'v';
        };

        foreach (['a', 'b', 'c', 'd'] as $key) {
            $cache->get($key, $factory);
        }

        $this->assertSame(3, $cache->size());
        $this->assertFalse($cache->has('a')); // evicted
        $this->assertTrue($cache->has('d'));

        // Refetching 'a' runs the factory again and re-enters it.
        $this->assertSame('v', $cache->get('a', $factory));
        $this->assertSame(5, $factoryCalls);
        $this->assertSame(3, $cache->size());
    }

    public function testPatternCacheTouchRefreshesLruOrder(): void
    {
        $cache = new PatternCache(3);
        $factory = fn () => 'v';

        foreach (['a', 'b', 'c'] as $key) {
            $cache->get($key, $factory);
        }
        $cache->get('a', $factory); // touch: 'a' becomes most-recent
        $cache->get('d', $factory); // evicts 'b' (now least-recent)

        $this->assertFalse($cache->has('b'));
        $this->assertTrue($cache->has('a'));
        $this->assertTrue($cache->has('c'));
        $this->assertTrue($cache->has('d'));
        $this->assertSame(3, $cache->size());
    }

    public function testPatternCacheClear(): void
    {
        $cache = new PatternCache(3);
        $cache->get('a', fn () => 'v');
        $cache->clear();

        $this->assertSame(0, $cache->size());
        $this->assertFalse($cache->has('a'));
    }
}
