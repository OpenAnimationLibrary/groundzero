#!/usr/bin/env ruby
# Count baseline PR checks, including matrix expansion and reusable workflows.
require 'pathname'
require 'yaml'

class CiBudgetError < StandardError; end

class CiBudget
  PR_EVENTS = %w[pull_request pull_request_target].freeze
  INTEGRATION_BRANCHES = %w[edition main].freeze
  INDIRECT_EVENTS = %w[workflow_run check_run check_suite].freeze
  LIMIT = 10

  def initialize(root)
    @root = Pathname.new(root).expand_path
    @workflows = {}
    Dir.glob(@root.join('.github/workflows/*.{yml,yaml}')).sort.each do |path|
      document = YAML.safe_load(File.read(path), aliases: false)
      raise CiBudgetError, "#{path}: workflow must be a mapping" unless document.is_a?(Hash)

      @workflows[Pathname.new(path)] = document
    end
    raise CiBudgetError, 'No workflow files found' if @workflows.empty?
  end

  def pull_request_counts
    counts_by_activity = Hash.new { |counts, activity| counts[activity] = Hash.new(0) }
    @workflows.each do |path, document|
      # Psych follows YAML 1.1, where an unquoted `on` key is boolean true.
      triggers = document.fetch('on', document[true])
      events = triggers.is_a?(Hash) ? triggers.keys : Array(triggers)
      validate_triggers(path, triggers, events)
      automatic_events = events & PR_EVENTS
      next if automatic_events.empty?

      count = workflow_count(path, [])
      automatic_events.each do |event|
        trigger = triggers[event] if triggers.is_a?(Hash)
        activities = trigger.is_a?(Hash) && trigger.key?('types') ? trigger['types'] : %w[opened reopened synchronize]
        unless activities.is_a?(Array) && !activities.empty?
          raise CiBudgetError, "#{path.basename}: PR activity types must be a nonempty array"
        end
        activities.uniq.each { |activity| counts_by_activity[activity][path.basename.to_s] += count }
      end
    end
    counts_by_activity.values.max_by { |counts| counts.values.sum } || {}
  end

  def validate_triggers(path, triggers, events)
    unless (events & INDIRECT_EVENTS).empty?
      raise CiBudgetError, "#{path.basename}: indirect check triggers need an explicit budget model"
    end
    return unless events.include?('push')

    push = triggers.is_a?(Hash) ? triggers['push'] : nil
    branches = push.is_a?(Hash) ? push['branches'] : nil
    return if tag_only_push?(push)
    allowed_branches = INTEGRATION_BRANCHES
    allowed_branches += ['ci/seexpr-*'] if path.basename.to_s == 'seexpr.yml'
    return if branches.is_a?(Array) && !branches.empty? &&
              (branches - allowed_branches).empty?

    raise CiBudgetError,
          "#{path.basename}: push branches must be restricted to edition/main; " \
          'use nightly or manual qualification for proposal branches'
  end

  def tag_only_push?(push)
    push.is_a?(Hash) && (push.key?('tags') || push.key?('tags-ignore')) &&
      !push.key?('branches') && !push.key?('branches-ignore')
  end

  def workflow_count(path, ancestors)
    raise CiBudgetError, "Reusable workflow cycle at #{path.basename}" if ancestors.include?(path)

    document = @workflows[path]
    raise CiBudgetError, "Missing local reusable workflow: #{path}" unless document

    jobs = document['jobs']
    unless jobs.is_a?(Hash) && !jobs.empty?
      raise CiBudgetError, "#{path.basename}: jobs must be a nonempty mapping"
    end

    jobs = baseline_jobs(jobs) if ancestors.empty? && path.basename.to_s == 'pr_checks.yml'
    jobs.sum do |name, job|
      raise CiBudgetError, "#{name}: job must be a mapping" unless job.is_a?(Hash)

      multiplier = matrix_count(job.fetch('strategy', {}).fetch('matrix', nil))
      multiplier * job_count(job, ancestors + [path])
    end
  end

  def baseline_jobs(jobs)
    # The edition wrapper reports the baseline result in one summary job.
    # Its advisory workflows run after that gate and have a separate purpose.
    gate = jobs.fetch('required')
    needed = Array(gate.fetch('needs'))
    if needed.empty? || !needed.all? { |name| jobs.key?(name) && name != 'required' } ||
       gate.key?('uses') || gate.key?('strategy')
      raise CiBudgetError, 'PR Checks: required must summarize existing baseline jobs'
    end

    advisory = jobs.reject { |name, _| name == 'required' || needed.include?(name) }
    baseline = jobs.slice(*needed)
    if baseline.values.any? { |job| (Array(job['needs']) - needed).any? }
      raise CiBudgetError, 'PR Checks: a baseline job depends on a job outside the baseline'
    end
    gated = ['required']
    until advisory.empty?
      ready = advisory.select do |_, job|
        (Array(job['needs']) & gated).any? &&
          !job.fetch('if', '').to_s.match?(/\b(?:always|failure|cancelled)\s*\(/i)
      end
      if ready.empty?
        raise CiBudgetError, 'PR Checks: advisory jobs must wait for successful baseline checks'
      end

      gated.concat(ready.keys)
      advisory = advisory.reject { |name, _| ready.key?(name) }
    end
    baseline
  end

  def job_count(job, ancestors)
    return 1 unless job.key?('uses')

    target = job['uses']
    unless target.is_a?(String) && target.match?(%r{\A\./\.github/workflows/[^/]+\.ya?ml\z})
      raise CiBudgetError, "Cannot statically count external/dynamic reusable workflow: #{target}"
    end

    workflow_count(@root.join(target).cleanpath, ancestors)
  end

  def matrix_count(matrix)
    return 1 if matrix.nil?
    raise CiBudgetError, 'Dynamic matrix cannot be counted statically' unless matrix.is_a?(Hash)

    check_static(matrix)
    axes = matrix.reject { |key, _| %w[include exclude].include?(key) }
    included = matrix_entries(matrix, 'include')
    excluded = matrix_entries(matrix, 'exclude')
    return included.length if axes.empty? && !included.empty?
    raise CiBudgetError, 'Empty matrix cannot be counted' if axes.empty?

    rows = matrix_rows(axes)
    rows.reject! { |row| excluded.any? { |entry| contains?(row, entry) } }
    added = included.count do |entry|
      rows.none? { |row| entry.all? { |key, value| !row.key?(key) || row[key] == value } }
    end
    rows.length + added
  end

  def matrix_entries(matrix, key)
    entries = matrix.fetch(key, [])
    unless entries.is_a?(Array) && entries.all? { |entry| entry.is_a?(Hash) }
      raise CiBudgetError, "matrix #{key} must be an array of mappings"
    end
    entries
  end

  def matrix_rows(axes)
    rows = [{}]
    axes.each do |key, values|
      unless values.is_a?(Array) && !values.empty?
        raise CiBudgetError, "matrix axis #{key} must be a nonempty array"
      end
      if rows.length * values.length > 256
        raise CiBudgetError, 'Matrix exceeds GitHub Actions 256-job limit'
      end

      rows = rows.flat_map { |row| values.map { |value| row.merge(key => value) } }
    end
    rows
  end

  def contains?(row, entry)
    entry.all? { |key, value| row.key?(key) && row[key] == value }
  end

  def check_static(value)
    case value
    when Hash
      value.each_value { |item| check_static(item) }
    when Array
      value.each { |item| check_static(item) }
    when String
      if value.include?('${{')
        raise CiBudgetError, 'Expression in matrix cannot be counted statically'
      end
    end
  end
end

if $PROGRAM_NAME == __FILE__
  begin
    root = ARGV.fetch(0, File.expand_path('..', __dir__))
    counts = CiBudget.new(root).pull_request_counts
    counts.each { |name, count| puts "#{name}: #{count}" }
    total = counts.values.sum
    puts "Automatic baseline PR checks: #{total}/#{CiBudget::LIMIT}"
    if total > CiBudget::LIMIT
      warn '::error title=CI budget::Consolidate baseline PR checks; the maximum is 10.'
      exit 1
    end
  rescue CiBudgetError, Psych::Exception, KeyError, NoMethodError => error
    warn "::error title=CI budget::#{error.message}"
    exit 1
  end
end
