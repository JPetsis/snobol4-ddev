<?php

declare(strict_types=1);

namespace Snobol\Tests;

use PHPUnit\Framework\TestCase;
use Snobol\PatternHelper;
use Snobol\Table;

/**
 * PatternHelper consistency tests (fix-php-binding-scan-findings group 7):
 * table-backed substitution, real compiler errors from fromAst, and an
 * options-sensitive pattern cache.
 */
final class PatternHelperConsistencyTest extends TestCase
{
    public function testTableSubstUsesTableBackedTemplates(): void
    {
        $table = new Table('STATE');
        $table->set('CA', 'California');
        $table->set('NY', 'New York');

        $out = PatternHelper::tableSubst(
            $table,
            "@x SPAN('ABCDEFGHIJKLMNOPQRSTUVWXYZ')",
            '$STATE[$v0]',
            'NY CA'
        );

        $this->assertSame('New York California', $out);
    }

    public function testTableSubstLeavesUnknownKeysAsMatches(): void
    {
        $table = new Table('STATE');
        $table->set('CA', 'California');

        // 'XX' is not in the table: the lookup degrades to an empty
        // substitution (the match is still consumed).
        $out = PatternHelper::tableSubst(
            $table,
            "@x SPAN('ABCDEFGHIJKLMNOPQRSTUVWXYZ')",
            '$STATE[$v0]',
            'XX'
        );

        $this->assertSame('', $out);
    }

    public function testFromAstSurfacesCompilerError(): void
    {
        try {
            PatternHelper::fromAst(['type' => 'lit']); // missing 'text'
            $this->fail('fromAst must throw for an invalid AST');
        } catch (\Exception $e) {
            // The compiler's own message must survive (as the cause), not be
            // replaced by the helper's generic wording.
            $this->assertNotSame('Failed to compile pattern from AST', $e->getMessage());
            $prev = $e->getPrevious();
            $this->assertNotNull($prev);
            $this->assertStringContainsString('Failed to convert PHP AST', $prev->getMessage());
        }
    }

    public function testHelperCacheDistinguishesOptions(): void
    {
        // Case-insensitive compile first (fills the cache slot)...
        $this->assertIsArray(
            PatternHelper::matchOnce("'abc'", 'ABCx', ['caseInsensitive' => true])
        );

        // ...then the same source WITHOUT options must NOT reuse it.
        $this->assertFalse(PatternHelper::matchOnce("'abc'", 'ABCx'));
    }

    public function testHelperCacheDistinguishesOptionsReverse(): void
    {
        // Sensitive compile first...
        $this->assertFalse(PatternHelper::matchOnce("'abc'", 'ABCx'));

        // ...then the case-insensitive variant must compile separately.
        $this->assertIsArray(
            PatternHelper::matchOnce("'abc'", 'ABCx', ['caseInsensitive' => true])
        );
    }
}
