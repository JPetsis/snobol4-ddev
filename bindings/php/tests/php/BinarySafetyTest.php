<?php

declare(strict_types=1);

namespace Snobol\Tests;

use PHPUnit\Framework\TestCase;
use Snobol\Array_;
use Snobol\Table;

/**
 * Binary-safety tests (fix-php-binding-scan-findings group 6):
 * NUL-safe table keys/values, NUL-safe Array_ values, and UTF-8
 * lpad/rpad/char handling.
 */
final class BinarySafetyTest extends TestCase
{
    public function testTableKeysDistinguishAfterEmbeddedNul(): void
    {
        $table = new Table();
        $keyA = "k\x00a";
        $keyB = "k\x00b";

        $table->set($keyA, 'A');
        $table->set($keyB, 'B');

        $this->assertTrue($table->has($keyA));
        $this->assertTrue($table->has($keyB));
        $this->assertSame('A', $table->get($keyA));
        $this->assertSame('B', $table->get($keyB));
        $this->assertFalse($table->has('k'));

        $this->assertTrue($table->delete($keyA));
        $this->assertFalse($table->has($keyA));
        $this->assertSame('B', $table->get($keyB)); // sibling survives
    }

    public function testTableValuesPreserveEmbeddedNul(): void
    {
        $table = new Table();
        $value = "v\x00x";

        $table->set('key', $value);
        $this->assertSame($value, $table->get('key'));
        $this->assertSame(3, strlen($table->get('key')));
    }

    public function testArrayValuesPreserveEmbeddedNul(): void
    {
        $arr = new Array_(4);
        $value = "a\x00b";

        $arr->set(1, $value);
        $this->assertSame($value, $arr->get(1));
        $this->assertSame(3, strlen($arr->get(1)));

        $values = $arr->values();
        $this->assertSame([$value], $values);
    }

    public function testLpadUsesFullUtf8PadCodepoint(): void
    {
        $this->assertSame('€x', snobol_text_lpad('x', 2, '€'));
        // Width counts codepoints, not bytes: pad '€' to width 3.
        $this->assertSame('€€x', snobol_text_lpad('x', 3, '€'));
        $this->assertSame('x', snobol_text_lpad('x', 1, '€'));
    }

    public function testRpadUsesFullUtf8PadCodepoint(): void
    {
        $this->assertSame('x€', snobol_text_rpad('x', 2, '€'));
        $this->assertSame('x€€', snobol_text_rpad('x', 3, '€'));
    }

    public function testCharRejectsOutOfRangeCodepoints(): void
    {
        $this->assertSame('A', snobol_text_char(65));
        $this->assertFalse(snobol_text_char(0x110000)); // > U+10FFFF
        $this->assertFalse(snobol_text_char(-1));
        $this->assertFalse(snobol_text_char(0xD800)); // surrogate
    }
}
