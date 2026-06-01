from setuptools import setup, find_packages

setup(
    name="lemonade-semantic-router",
    version="0.1.0",
    packages=find_packages(),
    install_requires=[
        "fastapi>=0.100.0",
        "uvicorn>=0.20.0",
        "pyyaml>=6.0",
        "numpy>=1.20.0",
        "transformers>=4.30.0",
        "torch>=2.0.0",
        "sentence-transformers>=2.2.0",
        "scikit-learn>=1.0.0",
    ],
    entry_points={
        "console_scripts": [
            "semantic-router=semantic_router.service:main",
        ],
    },
    python_requires=">=3.9",
)
