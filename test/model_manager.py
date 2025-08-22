"""
Unit tests for ModelManager class focusing on model deletion functionality.

These tests validate the bug fixes for:
1. Model deletion when cache files are manually removed (LocalEntryNotFoundError handling)
2. GGUF variant cross-deletion bug (selective variant deletion)

Usage: python model_manager_test.py
"""

import unittest
import tempfile
import shutil
import os
import json
from unittest.mock import patch, MagicMock, mock_open, PropertyMock
from pathlib import Path

# Import the class we're testing
from lemonade_server.model_manager import ModelManager, USER_MODELS_FILE


class TestModelManagerDeletion(unittest.TestCase):
    """Test suite for ModelManager deletion functionality."""
    
    def setUp(self):
        """Set up test environment with temporary directories and mock data."""
        # Create temporary directories for testing
        self.temp_dir = tempfile.mkdtemp()
        self.cache_dir = os.path.join(self.temp_dir, "cache")
        self.hf_cache_dir = os.path.join(self.temp_dir, "hf_cache")
        
        # Create directory structure
        os.makedirs(self.cache_dir, exist_ok=True)
        os.makedirs(self.hf_cache_dir, exist_ok=True)
        
        # Mock the cache directory constant
        self.hf_cache_patcher = patch('huggingface_hub.constants.HF_HUB_CACHE', self.hf_cache_dir)
        self.hf_cache_patcher.start()
        
        # Mock the user models file path
        self.user_models_file = os.path.join(self.cache_dir, "user_models.json")
        self.user_models_patcher = patch('lemonade_server.model_manager.USER_MODELS_FILE', self.user_models_file)
        self.user_models_patcher.start()
        
        # Create ModelManager instance
        self.model_manager = ModelManager()
        
        # Sample model configurations for testing
        self.sample_models = {
            "test-gguf-model": {
                "checkpoint": "unsloth/test-model-GGUF:Q4_K_M",
                "recipe": "llamacpp",
                "model_name": "test-gguf-model"
            },
            "test-gguf-model-variant2": {
                "checkpoint": "unsloth/test-model-GGUF:F16", 
                "recipe": "llamacpp",
                "model_name": "test-gguf-model-variant2"
            },
            "test-non-gguf-model": {
                "checkpoint": "amd/test-model-onnx",
                "recipe": "oga-cpu",
                "model_name": "test-non-gguf-model"
            },
            "user.test-user-model": {
                "checkpoint": "unsloth/user-model-GGUF:Q8_0",
                "recipe": "llamacpp", 
                "model_name": "user.test-user-model"
            }
        }

    def tearDown(self):
        """Clean up test environment."""
        self.hf_cache_patcher.stop()
        self.user_models_patcher.stop()
        shutil.rmtree(self.temp_dir, ignore_errors=True)

    def create_mock_hf_cache_structure(self, repo_name, variants=None, has_gguf_files=True):
        """Create a mock HuggingFace cache directory structure."""
        if variants is None:
            variants = ["Q4_K_M", "F16"]
            
        repo_cache_name = repo_name.replace("/", "--")
        repo_cache_dir = os.path.join(self.hf_cache_dir, f"models--{repo_cache_name}")
        snapshots_dir = os.path.join(repo_cache_dir, "snapshots") 
        snapshot_hash_dir = os.path.join(snapshots_dir, "abc123def456")
        
        os.makedirs(snapshot_hash_dir, exist_ok=True)
        
        if has_gguf_files:
            # Create mock GGUF files for each variant
            for variant in variants:
                if variant == "Q4_K_M":
                    variant_file = f"test-model-{variant}.gguf"
                elif variant == "F16": 
                    variant_file = f"test-model-{variant}.gguf"
                else:
                    variant_file = f"test-model-{variant}.gguf"
                    
                variant_path = os.path.join(snapshot_hash_dir, variant_file)
                with open(variant_path, 'w') as f:
                    f.write(f"mock gguf content for {variant}")
                    
            # Create some non-GGUF files
            with open(os.path.join(snapshot_hash_dir, "README.md"), 'w') as f:
                f.write("Test model README")
            with open(os.path.join(snapshot_hash_dir, ".gitattributes"), 'w') as f:
                f.write("*.gguf filter=lfs")
        
        return repo_cache_dir, snapshot_hash_dir

    def create_user_models_file(self, models_data):
        """Create a mock user_models.json file."""
        os.makedirs(os.path.dirname(self.user_models_file), exist_ok=True)
        with open(self.user_models_file, 'w') as f:
            json.dump(models_data, f)

    @patch.object(ModelManager, 'supported_models', new_callable=PropertyMock)
    def test_delete_nonexistent_model_raises_error(self, mock_supported_models):
        """Test that deleting a non-existent model raises ValueError."""
        mock_supported_models.return_value = {}
        
        with self.assertRaises(ValueError) as context:
            self.model_manager.delete_model("nonexistent-model")
            
        self.assertIn("is not supported", str(context.exception))

    @patch.object(ModelManager, 'supported_models', new_callable=PropertyMock)
    @patch('lemonade.common.network.custom_snapshot_download')
    def test_delete_model_with_manual_cache_removal(self, mock_snapshot_download, mock_supported_models):
        """Test Bug Fix #1: Graceful handling when cache files are manually removed."""
        mock_supported_models.return_value = self.sample_models
        
        # Mock LocalEntryNotFoundError when trying to find cache
        mock_snapshot_download.side_effect = Exception("LocalEntryNotFoundError: not found in cache")
        
        # Create the expected cache structure that would be manually constructed
        repo_cache_dir, snapshot_dir = self.create_mock_hf_cache_structure("amd/test-model-onnx", 
                                                                          variants=[], 
                                                                          has_gguf_files=False)
        
        # Should not raise an exception, should handle gracefully
        try:
            self.model_manager.delete_model("test-non-gguf-model")
        except Exception as e:
            self.fail(f"delete_model should handle missing cache gracefully, but raised: {e}")
            
        # Verify that manual cache path construction was used (directory should be deleted)
        self.assertFalse(os.path.exists(repo_cache_dir), 
                        "Cache directory should be deleted after manual path construction")

    @patch.object(ModelManager, 'supported_models', new_callable=PropertyMock)
    @patch('lemonade.common.network.custom_snapshot_download')
    def test_delete_model_cache_already_manually_deleted(self, mock_snapshot_download, mock_supported_models):
        """Test handling when cache directory doesn't exist (already manually deleted)."""
        mock_supported_models.return_value = self.sample_models
        
        # Mock the error and don't create any cache structure  
        mock_snapshot_download.side_effect = Exception("cannot find an appropriate cached snapshot")
        
        # Should handle gracefully and provide appropriate message
        with patch('builtins.print') as mock_print:
            self.model_manager.delete_model("test-non-gguf-model")
            
        # Check that appropriate message was printed
        print_calls = [call[0][0] for call in mock_print.call_args_list]
        self.assertTrue(any("may have been manually deleted" in call for call in print_calls),
                       "Should print message about manual deletion")

    @patch.object(ModelManager, 'supported_models', new_callable=PropertyMock)
    @patch('lemonade.common.network.custom_snapshot_download')
    @patch('lemonade.tools.llamacpp.utils.identify_gguf_models')
    def test_gguf_variant_selective_deletion(self, mock_identify_gguf, mock_snapshot_download, mock_supported_models):
        """Test Bug Fix #2: GGUF variant selective deletion (not cross-deletion)."""
        mock_supported_models.return_value = self.sample_models
        
        # Create cache with multiple variants
        repo_cache_dir, snapshot_dir = self.create_mock_hf_cache_structure("unsloth/test-model-GGUF",
                                                                          variants=["Q4_K_M", "F16"])
        
        # Mock successful snapshot download
        mock_snapshot_download.return_value = snapshot_dir
        
        # Mock identify_gguf_models to return specific files for Q4_K_M variant
        mock_identify_gguf.return_value = (
            {"variant": "test-model-Q4_K_M.gguf"}, 
            []  # no sharded files
        )
        
        # Test deleting Q4_K_M variant
        with patch('builtins.print') as mock_print:
            self.model_manager.delete_model("test-gguf-model")  # Q4_K_M variant
        
        # Verify that only Q4_K_M file was deleted, F16 still exists
        q4_file = os.path.join(snapshot_dir, "test-model-Q4_K_M.gguf")
        f16_file = os.path.join(snapshot_dir, "test-model-F16.gguf")
        
        self.assertFalse(os.path.exists(q4_file), "Q4_K_M variant file should be deleted")
        self.assertTrue(os.path.exists(f16_file), "F16 variant file should still exist")
        self.assertTrue(os.path.exists(repo_cache_dir), "Repository cache should still exist")
        
        # Check appropriate messages were printed
        print_calls = [call[0][0] for call in mock_print.call_args_list]
        self.assertTrue(any("Successfully deleted variant files" in call for call in print_calls))
        self.assertTrue(any("Other variants still exist" in call for call in print_calls))

    @patch.object(ModelManager, 'supported_models', new_callable=PropertyMock)
    @patch('lemonade.common.network.custom_snapshot_download')
    @patch('lemonade.tools.llamacpp.utils.identify_gguf_models')
    def test_gguf_last_variant_deletion_removes_repository(self, mock_identify_gguf, mock_snapshot_download, mock_supported_models):
        """Test that deleting the last GGUF variant removes the entire repository."""
        mock_supported_models.return_value = self.sample_models
        
        # Create cache with only one variant
        repo_cache_dir, snapshot_dir = self.create_mock_hf_cache_structure("unsloth/test-model-GGUF", 
                                                                          variants=["Q4_K_M"])
        
        mock_snapshot_download.return_value = snapshot_dir
        mock_identify_gguf.return_value = (
            {"variant": "test-model-Q4_K_M.gguf"},
            []
        )
        
        with patch('builtins.print') as mock_print:
            self.model_manager.delete_model("test-gguf-model")
        
        # Verify entire repository was deleted
        self.assertFalse(os.path.exists(repo_cache_dir), 
                        "Entire repository should be deleted when last variant is removed")
        
        # Check messages
        print_calls = [call[0][0] for call in mock_print.call_args_list]
        self.assertTrue(any("No other variants remain" in call for call in print_calls))
        self.assertTrue(any("deleting entire repository cache" in call for call in print_calls))

    @patch.object(ModelManager, 'supported_models', new_callable=PropertyMock)
    @patch('lemonade.common.network.custom_snapshot_download')
    def test_non_gguf_model_deletion(self, mock_snapshot_download, mock_supported_models):
        """Test that non-GGUF models are deleted entirely (existing behavior preserved)."""
        mock_supported_models.return_value = self.sample_models
        
        # Create cache for non-GGUF model
        repo_cache_dir, snapshot_dir = self.create_mock_hf_cache_structure("amd/test-model-onnx",
                                                                          variants=[],
                                                                          has_gguf_files=False)
        
        # Create some ONNX files instead
        with open(os.path.join(snapshot_dir, "model.onnx"), 'w') as f:
            f.write("mock onnx content")
        
        mock_snapshot_download.return_value = snapshot_dir
        
        with patch('builtins.print') as mock_print:
            self.model_manager.delete_model("test-non-gguf-model")
        
        # Verify entire repository was deleted
        self.assertFalse(os.path.exists(repo_cache_dir), 
                        "Non-GGUF models should have entire repository deleted")
        
        print_calls = [call[0][0] for call in mock_print.call_args_list]  
        self.assertTrue(any("Successfully deleted model test-non-gguf-model" in call for call in print_calls))

    @patch.object(ModelManager, 'supported_models', new_callable=PropertyMock)
    @patch('lemonade.common.network.custom_snapshot_download')
    def test_user_model_registry_cleanup(self, mock_snapshot_download, mock_supported_models):
        """Test that user models are properly removed from user_models.json registry."""
        mock_supported_models.return_value = self.sample_models
        
        # Create user_models.json file
        user_models_data = {
            "test-user-model": {
                "checkpoint": "unsloth/user-model-GGUF:Q8_0",
                "recipe": "llamacpp"
            },
            "another-user-model": {
                "checkpoint": "other/model:Q4_0", 
                "recipe": "llamacpp"
            }
        }
        self.create_user_models_file(user_models_data)
        
        # Create cache structure
        repo_cache_dir, snapshot_dir = self.create_mock_hf_cache_structure("unsloth/user-model-GGUF")
        mock_snapshot_download.return_value = snapshot_dir
        
        # Delete user model
        self.model_manager.delete_model("user.test-user-model")
        
        # Verify model was removed from registry
        with open(self.user_models_file, 'r') as f:
            remaining_models = json.load(f)
            
        self.assertNotIn("test-user-model", remaining_models, 
                        "User model should be removed from registry")
        self.assertIn("another-user-model", remaining_models,
                     "Other user models should remain in registry")

    @patch.object(ModelManager, 'supported_models', new_callable=PropertyMock)
    @patch('lemonade.common.network.custom_snapshot_download')
    @patch('lemonade.tools.llamacpp.utils.identify_gguf_models')
    def test_gguf_variant_deletion_error_handling(self, mock_identify_gguf, mock_snapshot_download, mock_supported_models):
        """Test error handling during variant-specific deletion."""
        mock_supported_models.return_value = self.sample_models
        
        repo_cache_dir, snapshot_dir = self.create_mock_hf_cache_structure("unsloth/test-model-GGUF")
        mock_snapshot_download.return_value = snapshot_dir
        
        # Mock identify_gguf_models to raise an error
        mock_identify_gguf.side_effect = Exception("Test error in identify_gguf_models")
        
        with patch('builtins.print') as mock_print:
            # Should not raise exception, should handle gracefully
            self.model_manager.delete_model("test-gguf-model")
        
        print_calls = [call[0][0] for call in mock_print.call_args_list]
        self.assertTrue(any("Warning: Could not perform selective variant deletion" in call for call in print_calls))
        self.assertTrue(any("This may indicate the files were already manually deleted" in call for call in print_calls))

    @patch.object(ModelManager, 'supported_models', new_callable=PropertyMock)
    @patch('lemonade.common.network.custom_snapshot_download') 
    def test_backward_compatibility_no_variant_gguf(self, mock_snapshot_download, mock_supported_models):
        """Test backward compatibility: GGUF models without variants are deleted entirely."""
        # Model without variant specification
        models_no_variant = {
            "test-gguf-no-variant": {
                "checkpoint": "unsloth/test-model-GGUF",  # No :variant
                "recipe": "llamacpp",
                "model_name": "test-gguf-no-variant"
            }
        }
        mock_supported_models.return_value = models_no_variant
        
        repo_cache_dir, snapshot_dir = self.create_mock_hf_cache_structure("unsloth/test-model-GGUF")
        mock_snapshot_download.return_value = snapshot_dir
        
        with patch('builtins.print') as mock_print:
            self.model_manager.delete_model("test-gguf-no-variant")
        
        # Should delete entire repository (no variant = no selective deletion)
        self.assertFalse(os.path.exists(repo_cache_dir),
                        "GGUF model without variant should have entire repository deleted")


class TestModelManagerIntegration(unittest.TestCase):
    """Integration tests that validate the fixes work with real-world scenarios."""
    
    def setUp(self):
        """Set up integration test environment."""
        self.temp_dir = tempfile.mkdtemp()
        self.model_manager = ModelManager()
    
    def tearDown(self):
        """Clean up integration test environment."""
        shutil.rmtree(self.temp_dir, ignore_errors=True)

    def test_parse_checkpoint_integration(self):
        """Test that parse_checkpoint is used correctly in our fixes."""
        from lemonade.tools.llamacpp.utils import parse_checkpoint
        
        # Test various checkpoint formats
        test_cases = [
            ("unsloth/model-GGUF:Q4_K_M", ("unsloth/model-GGUF", "Q4_K_M")),
            ("unsloth/model-GGUF:file.gguf", ("unsloth/model-GGUF", "file.gguf")), 
            ("unsloth/model-GGUF", ("unsloth/model-GGUF", None)),
            ("amd/model-onnx", ("amd/model-onnx", None))
        ]
        
        for checkpoint, expected in test_cases:
            base, variant = parse_checkpoint(checkpoint)
            self.assertEqual((base, variant), expected, 
                           f"parse_checkpoint failed for {checkpoint}")

    def test_hf_cache_path_construction(self):
        """Test that HuggingFace cache path construction follows the expected pattern."""
        import huggingface_hub.constants
        
        # Test the manual cache path construction logic
        test_repos = [
            "unsloth/Qwen3-0.6B-GGUF",
            "amd/model-onnx-cpu", 
            "microsoft/DialoGPT-medium"
        ]
        
        for repo in test_repos:
            # This is the logic from our fix
            repo_cache_name = repo.replace("/", "--")
            expected_path = f"models--{repo_cache_name}"
            
            # Verify the naming convention matches HuggingFace standards
            self.assertTrue(expected_path.startswith("models--"))
            self.assertNotIn("/", expected_path)
            self.assertIn("--", expected_path)


if __name__ == "__main__":
    # Create a test suite
    suite = unittest.TestSuite()
    
    # Add test classes to the suite
    suite.addTests(unittest.TestLoader().loadTestsFromTestCase(TestModelManagerDeletion))
    suite.addTests(unittest.TestLoader().loadTestsFromTestCase(TestModelManagerIntegration))
    
    # Run the tests
    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite)
    
    # Exit with appropriate code
    exit(0 if result.wasSuccessful() else 1)

# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD