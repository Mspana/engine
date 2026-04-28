# API Key Configuration

The AI module supports loading API keys from environment variables or a `.env` file for security and convenience.

## Setup Methods

### Option 1: Environment Variables (Recommended for CI/CD)

Set system environment variables:

**Windows:**
```powershell
$env:OPENAI_API_KEY="sk-..."
$env:GEMINI_API_KEY="..."
$env:XAI_API_KEY="..."
$env:ANTHROPIC_API_KEY="sk-ant-..."
$env:DEEPINFRA_API_KEY="..."
$env:PARASAIL_API_KEY="..."
$env:CLARIFAI_API_KEY="..."
```

**Linux/Mac:**
```bash
export OPENAI_API_KEY="sk-..."
export GEMINI_API_KEY="..."
export XAI_API_KEY="..."
export ANTHROPIC_API_KEY="sk-ant-..."
export DEEPINFRA_API_KEY="..."
export PARASAIL_API_KEY="..."
export CLARIFAI_API_KEY="..."
```

### Option 2: .env File (Recommended for Development)

1. Copy `.env.example` to your **project root** (not the engine root) and rename it to `.env`:
   ```
   cp modules/ai/.env.example /path/to/your/godot/project/.env
   ```

2. Edit `.env` and add your actual API keys:
   ```env
   OPENAI_API_KEY=sk-proj-xxxxxxxxxxxxx
   GEMINI_API_KEY=AIzaSyxxxxxxxxxxxxxx
   XAI_API_KEY=xai-xxxxxxxxxxxxx
   ANTHROPIC_API_KEY=sk-ant-xxxxxxxxxxxxx
   DEEPINFRA_API_KEY=xxxxxxxxxxxxx
   PARASAIL_API_KEY=xxxxxxxxxxxxx
   CLARIFAI_API_KEY=xxxxxxxxxxxxx
   ```

3. **Important:** Add `.env` to your project's `.gitignore` to avoid committing secrets:
   ```gitignore
   # Add this to your project's .gitignore
   .env
   ```

### Option 3: Manual Configuration (For Testing)

You can still set API keys manually in code:

```gdscript
var openai = OpenAIProvider.new()
openai.api_key = "sk-your-key-here"
AI.provider = openai
```

## How It Works

When you create a provider (e.g., `OpenAIProvider.new()`), it automatically:

1. **First**, checks for a system environment variable (e.g., `OPENAI_API_KEY`)
2. **Then**, if not found, looks for a `.env` file in your project root (`res://.env`)
3. **Finally**, if still not found, the API key remains empty (you can set it manually)

The `.env` file is parsed simply:
- Lines starting with `#` are comments
- Format: `KEY=VALUE`
- Quotes around values are automatically removed
- Empty lines are ignored

## Environment Variable Names

- **OpenAI**: `OPENAI_API_KEY`
- **Gemini**: `GEMINI_API_KEY`
- **x.ai (Grok)**: `XAI_API_KEY`
- **Anthropic (Claude)**: `ANTHROPIC_API_KEY`
- **DeepInfra** (Kimi K2.5 and other open-source models): `DEEPINFRA_API_KEY`
- **Parasail** (Kimi K2.6, multimodal): `PARASAIL_API_KEY`
- **Clarifai** (Kimi K2.6, faster alternate host): `CLARIFAI_API_KEY`

## Security Best Practices

1. **Never commit API keys** to version control
2. **Use environment variables** in production/CI environments
3. **Use .env files** only for local development
4. **Always add .env to .gitignore**
5. **Rotate keys** if accidentally exposed

## Getting API Keys

- **OpenAI**: https://platform.openai.com/api-keys
- **Google Gemini**: https://makersuite.google.com/app/apikey
- **x.ai (Grok)**: https://console.x.ai/
- **Anthropic (Claude)**: https://console.anthropic.com/settings/keys
- **DeepInfra**: https://deepinfra.com/dash/api_keys
- **Parasail**: https://parasail.io/
- **Clarifai**: https://clarifai.com/settings/security

## Example Usage

```gdscript
# No manual configuration needed if .env is set up
var openai = OpenAIProvider.new()  # Automatically loads OPENAI_API_KEY
AI.provider = openai
AI.request_actions("Create a player sprite")
```

## Troubleshooting

If your API key isn't loading:

1. Check that `.env` is in your **project root** (`res://.env`), not the engine root
2. Verify the key names match exactly: `OPENAI_API_KEY`, `GEMINI_API_KEY`, `XAI_API_KEY`
3. Make sure there are no extra spaces around the `=` sign
4. Check the console for "Loaded API key from..." messages
5. Try setting the key manually to confirm the provider works

## Console Output

When a key is successfully loaded, you'll see:
```
AIProvider: Loaded API key from environment variable OPENAI_API_KEY
```
or
```
AIProvider: Loaded API key from .env file for OPENAI_API_KEY
```

If no key is found, there's no warning (it's optional).

