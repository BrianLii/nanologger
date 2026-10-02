#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
configured_region=${AWS_REGION:-$(aws configure get region || true)}
aws_region=${configured_region:-us-east-2}
stack_name=${NANOLOGGER_AWS_STACK_NAME:-nanologger-benchmark}
instance_type=c7i.xlarge
template_file="$project_root/infra/aws-benchmark.yml"
readonly project_root aws_region stack_name instance_type template_file

aws_cli=(aws --region "$aws_region")
readonly -a aws_cli

fail() {
  echo "$1" >&2
  exit 1
}

find_default_vpc() {
  local vpc_id
  vpc_id=$(
    "${aws_cli[@]}" ec2 describe-vpcs \
      --filters Name=is-default,Values=true \
      --query 'Vpcs[0].VpcId' \
      --output text
  )
  [[ -n "$vpc_id" && "$vpc_id" != "None" ]] || fail "No default VPC found in $aws_region"
  printf '%s\n' "$vpc_id"
}

find_benchmark_subnet() {
  local availability_zone candidate
  while IFS= read -r availability_zone; do
    [[ -n "$availability_zone" ]] || continue
    candidate=$(
      "${aws_cli[@]}" ec2 describe-subnets \
        --filters Name=default-for-az,Values=true \
                  Name=availability-zone,Values="$availability_zone" \
        --query 'Subnets[?MapPublicIpOnLaunch].SubnetId | [0]' \
        --output text
    )
    if [[ -n "$candidate" && "$candidate" != "None" ]]; then
      printf '%s\n' "$candidate"
      return
    fi
  done < <(
    "${aws_cli[@]}" ec2 describe-instance-type-offerings \
      --location-type availability-zone \
      --filters Name=instance-type,Values="$instance_type" \
      --query 'sort_by(InstanceTypeOfferings,&Location)[].Location' \
      --output text \
      | tr '\t' '\n'
  )
  fail "No public default subnet offers $instance_type in $aws_region"
}

deploy_stack() {
  local vpc_id=$1
  local subnet_id=$2

  "${aws_cli[@]}" cloudformation deploy \
    --stack-name "$stack_name" \
    --template-file "$template_file" \
    --capabilities CAPABILITY_IAM \
    --parameter-overrides VpcId="$vpc_id" SubnetId="$subnet_id" \
    --no-fail-on-empty-changeset
}

get_instance_id() {
  "${aws_cli[@]}" cloudformation describe-stacks \
    --stack-name "$stack_name" \
    --query 'Stacks[0].Outputs[?OutputKey==`InstanceId`].OutputValue | [0]' \
    --output text
}

vpc_id=$(find_default_vpc)
subnet_id=$(find_benchmark_subnet)
deploy_stack "$vpc_id" "$subnet_id"
instance_id=$(get_instance_id)

echo "AWS benchmark VM ready: $instance_id ($instance_type, $aws_region)"
