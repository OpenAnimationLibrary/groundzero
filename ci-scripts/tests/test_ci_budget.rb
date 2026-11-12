require 'minitest/autorun'
require 'tmpdir'
require 'fileutils'
require 'open3'
require 'rbconfig'
require File.expand_path('../check_ci_budget', __dir__)

class CiBudgetTest < Minitest::Test
  def with_workflows(workflows)
    Dir.mktmpdir('ci-budget') do |root|
      directory = File.join(root, '.github/workflows')
      FileUtils.mkdir_p(directory)
      workflows.each do |name, document|
        File.write(File.join(directory, name), document.to_yaml)
      end
      yield CiBudget.new(root)
    end
  end

  def workflow(jobs, event = 'pull_request')
    { 'on' => { event => nil }, 'jobs' => jobs }
  end

  def test_counts_nested_workflows_and_include_only_matrix
    workflows = {
      'pr.yml' => workflow({
        'build' => { 'uses' => './.github/workflows/build.yaml' },
        'lint' => { 'runs-on' => 'ubuntu-latest' }
      }),
      'build.yaml' => workflow({
        'matrix' => { 'strategy' => { 'matrix' => {
          'include' => [{ 'os' => 'linux' }, { 'os' => 'windows' }]
        } } },
        'package' => { 'uses' => './.github/workflows/package.yml' }
      }, 'workflow_call'),
      'package.yml' => workflow({ 'smoke' => {} }, 'workflow_call')
    }
    with_workflows(workflows) do |budget|
      assert_equal({ 'pr.yml' => 4 }, budget.pull_request_counts)
    end
  end

  def test_counts_skipped_jobs_and_target_events_but_not_manual_or_scheduled
    workflows = {
      'pr.yml' => workflow({ 'check' => { 'if' => false } }),
      'target.yml' => workflow({ 'check' => {} }, 'pull_request_target'),
      'manual.yml' => workflow({ 'check' => {} }, 'workflow_dispatch'),
      'nightly.yml' => workflow({ 'check' => {} }, 'schedule')
    }
    with_workflows(workflows) do |budget|
      assert_equal({ 'pr.yml' => 1, 'target.yml' => 1 }, budget.pull_request_counts)
    end
  end

  def test_counts_only_workflows_that_run_for_the_same_activity
    workflows = {
      'build.yml' => workflow({ 'build' => {}, 'smoke' => {} }),
      'scan.yml' => { 'on' => { 'pull_request' => { 'types' => ['edited'] } },
                      'jobs' => { 'scan' => {} } }
    }
    with_workflows(workflows) do |budget|
      assert_equal({ 'build.yml' => 2 }, budget.pull_request_counts)
    end
  end

  def test_counts_each_pull_request_event
    document = workflow({ 'check' => {} })
    document['on']['pull_request_target'] = nil
    with_workflows({ 'pr.yml' => document }) do |budget|
      assert_equal 2, budget.pull_request_counts.values.sum
    end
  end

  def test_rejects_additional_automatic_proposal_branch_checks
    triggers = [
      'push', ['push', 'pull_request'],
      { 'push' => nil }, { 'push' => { 'paths' => ['doc/**'] } },
      { 'push' => { 'branches' => ['ci/*'] } },
      { 'push' => { 'branches-ignore' => ['edition'] } },
      { 'workflow_run' => { 'workflows' => ['Linux Build'], 'types' => ['completed'] } },
      { 'check_run' => nil }, { 'check_suite' => nil }
    ]
    triggers.each do |trigger|
      document = { 'on' => trigger, 'jobs' => { 'check' => {} } }
      with_workflows({ 'extra.yml' => document }) do |budget|
        assert_raises(CiBudgetError) { budget.pull_request_counts }
      end
    end
  end

  def test_allows_integration_branch_and_tag_pushes_outside_pr_budget
    workflows = {
      'integration.yml' => { 'on' => { 'push' => { 'branches' => %w[edition main] } } },
      'tags.yml' => { 'on' => { 'push' => { 'tags' => ['v*'] } } },
      'other-tags.yml' => { 'on' => { 'push' => { 'tags-ignore' => ['v*'] } } }
    }
    with_workflows(workflows) do |budget|
      assert_empty budget.pull_request_counts
    end
  end

  def test_multiplies_reusable_workflows_by_caller_matrix
    caller = { 'uses' => './.github/workflows/build.yml',
               'strategy' => { 'matrix' => { 'arch' => %w[x64 arm64] } } }
    workflows = {
      'pr.yml' => workflow({ 'call' => caller }),
      'build.yml' => workflow({ 'build' => {}, 'test' => {} }, 'workflow_call')
    }
    with_workflows(workflows) do |budget|
      assert_equal({ 'pr.yml' => 4 }, budget.pull_request_counts)
    end
  end

  def test_unquoted_on_key
    document = { true => { 'pull_request' => nil }, 'jobs' => { 'check' => {} } }
    with_workflows({ 'pr.yml' => document }) do |budget|
      assert_equal 1, budget.pull_request_counts.values.sum
    end
  end

  def test_matrix_include_enriches_existing_rows_without_extra_checks
    with_workflows({ 'pr.yml' => workflow({ 'check' => {} }) }) do |budget|
      matrix = { 'compiler' => ['gcc'], 'include' => [{ 'compiler' => 'gcc', 'cc' => 'gcc' }] }
      assert_equal 1, budget.matrix_count(matrix)
    end
  end

  def test_matrix_cartesian_product_excludes_and_new_includes
    with_workflows({ 'pr.yml' => workflow({ 'check' => {} }) }) do |budget|
      matrix = {
        'os' => %w[linux windows], 'qt' => [5, 6],
        'exclude' => [{ 'os' => 'windows', 'qt' => 5 }],
        'include' => [{ 'os' => 'macos', 'qt' => 6 }, { 'os' => 'linux', 'label' => 'native' }]
      }
      assert_equal 4, budget.matrix_count(matrix)
    end
  end

  def test_counts_includes_against_original_rows_and_can_restore_excluded_rows
    with_workflows({ 'pr.yml' => workflow({ 'check' => {} }) }) do |budget|
      matrix = {
        'os' => %w[linux windows], 'exclude' => [{ 'os' => 'windows' }],
        'include' => [{ 'os' => 'windows' }, { 'os' => 'macos' },
                      { 'os' => 'macos', 'label' => 'extra' }]
      }
      assert_equal 4, budget.matrix_count(matrix)
    end
  end

  def test_rejects_invalid_matrix_shapes_and_oversized_cartesian_product
    matrices = [
      {}, { 'os' => [] }, { 'os' => 'linux' },
      { 'os' => ['linux'], 'include' => {} },
      { 'os' => ['linux'], 'exclude' => ['linux'] },
      { 'os' => (1..257).to_a }
    ]
    with_workflows({ 'pr.yml' => workflow({ 'check' => {} }) }) do |budget|
      matrices.each do |matrix|
        assert_raises(CiBudgetError) { budget.matrix_count(matrix) }
      end
    end
  end

  def test_rejects_missing_or_malformed_workflows_and_jobs
    assert_raises(CiBudgetError) { with_workflows({}) { flunk 'No workflows must fail' } }
    assert_raises(CiBudgetError) do
      with_workflows({ 'pr.yml' => [] }) { flunk 'A workflow must be a mapping' }
    end
    [nil, {}, { 'check' => nil }].each do |jobs|
      with_workflows({ 'pr.yml' => workflow(jobs) }) do |budget|
        assert_raises(CiBudgetError) { budget.pull_request_counts }
      end
    end
  end

  def test_rejects_dynamic_matrix_and_missing_or_external_workflows
    bad_jobs = [
      { 'strategy' => { 'matrix' => '${{ fromJSON(needs.setup.outputs.matrix) }}' } },
      { 'strategy' => { 'matrix' => { 'os' => ['${{ inputs.runner }}'] } } },
      { 'uses' => './.github/workflows/missing.yml' },
      { 'uses' => 'example/repo/.github/workflows/check.yml@main' }
    ]
    bad_jobs.each do |job|
      with_workflows({ 'pr.yml' => workflow({ 'check' => job }) }) do |budget|
        assert_raises(CiBudgetError) { budget.pull_request_counts }
      end
    end
  end

  def test_rejects_reusable_cycles
    workflows = {
      'pr.yml' => workflow({ 'check' => { 'uses' => './.github/workflows/pr.yml' } })
    }
    with_workflows(workflows) do |budget|
      assert_raises(CiBudgetError) { budget.pull_request_counts }
    end
  end

  def test_edition_wrapper_counts_baseline_and_requires_advisory_gate
    jobs = {
      'linux' => { 'runs-on' => 'ubuntu-latest' },
      'required' => { 'needs' => ['linux'], 'runs-on' => 'ubuntu-latest' },
      'scope' => { 'needs' => 'required', 'runs-on' => 'ubuntu-latest' },
      'optional' => { 'needs' => 'scope', 'uses' => './.github/workflows/optional.yml' }
    }
    optional = workflow({
      'dynamic' => { 'strategy' => { 'matrix' => '${{ fromJSON(inputs.matrix) }}' } }
    }, 'workflow_call')
    with_workflows({ 'pr_checks.yml' => workflow(jobs), 'optional.yml' => optional }) do |budget|
      assert_equal({ 'pr_checks.yml' => 1 }, budget.pull_request_counts)
    end

    jobs['optional']['if'] = 'always()'
    with_workflows({ 'pr_checks.yml' => workflow(jobs), 'optional.yml' => optional }) do |budget|
      assert_raises(CiBudgetError) { budget.pull_request_counts }
    end
  end

  def test_allows_dedicated_seexpr_qualification_branch
    document = { 'on' => { 'push' => { 'branches' => ['edition', 'ci/seexpr-*'] } } }
    with_workflows({ 'seexpr.yml' => document }) do |budget|
      assert_empty budget.pull_request_counts
    end
  end

  def test_cli_enforces_ten_check_boundary
    Dir.mktmpdir('ci-budget-limit') do |root|
      directory = File.join(root, '.github/workflows')
      FileUtils.mkdir_p(directory)
      script = File.expand_path('../check_ci_budget.rb', __dir__)
      [9, 10, 11].each do |count|
        jobs = (1..count).to_h { |index| ["check#{index}", {}] }
        File.write(File.join(directory, 'pr.yml'), workflow(jobs).to_yaml)
        output, result = Open3.capture2e(RbConfig.ruby, script, root)
        assert_equal count <= 10, result.success?, output
        assert_includes output, "Automatic baseline PR checks: #{count}/10"
        if count > 10
          assert_includes output, 'maximum is 10'
        else
          refute_includes output, '::error'
        end
      end
    end
  end
end
