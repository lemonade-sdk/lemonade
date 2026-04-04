# Embeddable Lemonade: Models

This guide covers how `lemond` discovers, exposes, and bundles models in an embeddable deployment.

Contents:

- [Model Organization](#model-organization)
    - [Sharing Models With Other Apps](#sharing-models-with-other-apps)
    - [Private Models](#private-models)
    - [Importing Models to `lemond`](#importing-models-to-lemond)
- [Customization](#customization)
    - [Changing the Built-In Models List](#changing-the-built-in-models-list)
    - [Per-Model Load Options](#per-model-load-options)
- [Bundling Models](#bundling-models)

## Model Organization

`lemond`'s configuration has two properties, `models_dir` and `extra_models_dir`, that determine where `lemond` will look when listing, pulling, and loading models.

- `models_dir` is the primary model store, where `lemond` will `pull` models to.
- `extra_models_dir` is a search path for GGUF LLMs that can be imported into `lemond`.

### Sharing Models With Other Apps

The default value for `models_dir` is `"auto"`, which means "respect my user's `HF_HOME` and `HF_HUB_CACHE` environment variables, in accordance with the Hugging Face Hub standard." If those environment variables are not set, it defaults to `~/.cache/huggingface/hub`.

The benefit of `models_dir="auto"` is that models are placed in a common location on your user's drive, which means they can be shared across applications. However, the downside is that other applications could manage models that your app relies on.

### Private Models

You can keep models completely private to your `lemond` instance by changing `models_dir` to be inside of the `lemond` directory:

=== "Windows (cmd.exe)"

    ```cmd
    REM Start lemond for configuration
    lemond.exe ./

    REM Set models_dir to be inside the lemond directory
    lemonade.exe config set models_dir="./models"
    ```

=== "Linux (bash)"

    ```bash
    # Start lemond for configuration
    ./lemond ./

    # Set models_dir to be inside the lemond directory
    ./lemonade config set models_dir="./models"
    ```

You can test whether models end up at the desired location with a pull command:

=== "Windows (cmd.exe)"

    ```cmd
    lemonade.exe pull Qwen3-0.6B-GGUF
    dir models
    ```

=== "Linux (bash)"

    ```bash
    ./lemonade pull Qwen3-0.6B-GGUF
    ls models
    ```

Which should return:

```
models--unsloth--Qwen3-0.6B-GGUF
```

> Note: if you need to use a GGUF LLM that is not available on huggingface.co, you can use GGUF files acquired through other means using the `extra_models_dir` configuration discussed in [Importing Models to `lemond`](#importing-models-to-lemond).

### Importing Models to `lemond`

You may want to share models within your app if `lemond` is not your only inference provider for GGUF LLMs.

There is also a scenario where `lemond`'s Hugging Face Hub-based `pull` methodology is not able to obtain the models you need, for example:
- Hugging Face Hub is blocked in your users' locality.
- Your GGUF files are not on Hugging Face Hub.

To address all of these scenarios, `lemond` can recursively search a directory for GGUF files using the `extra_models_dir` configuration.

For example, let's copy the GGUF file from the [Private Models](#private-models) example to another folder so that we have an arbitrary GGUF LLM to work with:

=== "Windows (cmd.exe)"

    ```cmd
    mkdir example_extra_models_dir
    copy models\models--unsloth--Qwen3-0.6B-GGUF\snapshots\50968a4468ef4233ed78cd7c3de230dd1d61a56b\Qwen3-0.6B-Q4_0.gguf example_extra_models_dir\my_custom_model.gguf
    ```

=== "Linux (bash)"

    ```bash
    mkdir example_extra_models_dir
    cp models/models--unsloth--Qwen3-0.6B-GGUF/snapshots/50968a4468ef4233ed78cd7c3de230dd1d61a56b/Qwen3-0.6B-Q4_0.gguf example_extra_models_dir/my_custom_model.gguf
    ```

Now we have a GGUF, `my_custom_model.gguf`, in a non-standard directory layout with a non-standard name.

To import it into `lemond`:

=== "Windows (cmd.exe)"

    ```cmd
    REM Start lemond for configuration
    lemond.exe ./

    REM Set the extra_models_dir search path
    lemonade.exe config set extra_models_dir="./example_extra_models_dir"
    ```

=== "Linux (bash)"

    ```bash
    # Start lemond for configuration
    ./lemond ./

    # Set the extra_models_dir search path
    ./lemonade config set extra_models_dir="./example_extra_models_dir"
    ```

You can test whether it worked like this:

=== "Windows (cmd.exe)"

    ```cmd
    lemonade.exe list | findstr custom
    ```

=== "Linux (bash)"

    ```bash
    ./lemonade list | grep custom
    ```

Which should return:

```
extra.my_custom_model.gguf              Yes         llamacpp
```

> Note: models imported via `extra_models_dir` will have the `extra.` prefix in the `list` command and `/v1/models` endpoint.

> Tip: `extra_models_dir` can be a relative path to any location within your app's package, or any absolute path on your user's system. It searches recursively and can import many GGUFs from a single directory tree.

## Customization

### Changing the Built-In Models List

`lemond` has a built-in list of over 100 suggested models in `resources/server_models.json`. This list determines the models that are available by name in the `/v1/models`, `/v/1/pull`, and `/v1/delete` endpoints at run time.

You can print the list with:

=== "Windows (cmd.exe)"

    ```cmd
    lemond.exe ./

    lemonade.exe list
    ```

=== "Linux (bash)"

    ```bash
    ./lemond ./

    ./lemonade list
    ```

You can customize this list at packaging time by editing `server_models.json`. You can remove any entry to make it inaccessible to your users, or you can add entries to introduce new options.

> Note: `lemond` needs to be restarted for `server_models.json` changes to take effect.

For example, if you wanted to add [OmniCoder-9B](https://huggingface.co/Tesslate/OmniCoder-9B-GGUF) to your application, add this entry to `server_models.json`:

```
"OmniCoder-9B-GGUF": {
    "checkpoint": "Tesslate/OmniCoder-9B-GGUF:omnicoder-9b-q4_k_m.gguf",
    "recipe": "llamacpp",
    "suggested": true,
    "labels": [
        "tool-calling"
    ],
    "size": 5.74
}
```

You can learn more in the [Custom Models](../server/custom-models.md) guide, including how to enable your users to register their own custom models at runtime.

### Per-Model Load Options

You may want to customize how specific models will load on your user's system. You can do this with the `recipe_options.json` file. Learn more in the [Custom Models](../server/custom-models.md) guide.

## Bundling Models

You can leverage `lemond` to include model files in your app's installer. To do so, follow the same instructions as the [Private Models](#private-models) section, and then package the `models` folder into your app's installer.
