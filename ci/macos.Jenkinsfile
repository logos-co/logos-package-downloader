#!/usr/bin/env groovy

library 'status-jenkins-lib@v1.9.48'

def isPRBuild = utils.isPRBuild()

pipeline {
  agent { label "macos && ${getArch()} && nix-2.33" }

  options {
    timestamps()
    ansiColor('xterm')
    timeout(time: 45, unit: 'MINUTES')
    buildDiscarder(logRotator(
      numToKeepStr: '10',
      daysToKeepStr: '30',
      artifactNumToKeepStr: '1',
    ))
    disableConcurrentBuilds(
      abortPrevious: isPRBuild
    )
    /* Allows combined build to copy */
    copyArtifactPermission('/logos/*')
  }

  environment {
    PLATFORM = "macos/${getArch()}"
    ARTIFACT = "pkg/${utils.pkgFilename(name: 'logos-package-downloader', type: 'macos', ext: 'tar.gz', arch: getArch())}"
  }

  stages {
    stage('Build') {
      steps { script {
        nix.flake('cli-bundle-dir')
      } }
    }

    stage('Smoke Test') {
      steps {
        sh 'result/bin/lgpd --version'
      }
    }

    stage('Sign & Notarize') {
      steps {
        script {
          logos.codesign(
            dirPath: 'result',
            outputPath: 'signed',
            mode: 'both',
            timeout: '30m'
          )
        }
      }
    }

    stage('Package') {
      steps {
        sh "./scripts/create-tarball.sh --dir signed --output ${env.ARTIFACT}"
      }
    }

    stage('Upload') {
      steps { script {
        env.PKG_URL = s5cmd.upload(env.ARTIFACT)
        jenkins.setBuildDesc(TARGZ: env.PKG_URL)
      } }
    }

    stage('Archive') {
      steps { script {
        archiveArtifacts(env.ARTIFACT)
      } }
    }
  }

  post {
    success { script { github.notifyPR(true) } }
    failure { script { github.notifyPR(false) } }
    cleanup {
      cleanWs(disableDeferredWipeout: true)
      dir(env.WORKSPACE_TMP) { deleteDir() }
    }
  }
}

def getArch() {
  def tokens = Thread.currentThread().getName().split('/')
  for (def arch in ['x86_64', 'aarch64']) {
    if (tokens.contains(arch)) { return arch }
  }
}